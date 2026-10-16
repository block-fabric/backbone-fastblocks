// The page of the dialogs of the host (dialog.ts): text only, the marks that turn text around written out, the look of
// the app (built-in themes and the user's, an unreadable one drawn as its base), the keys, and the order of its bridge.
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { applyDialogLook, ARM_MS, readData, readable, render, start, wire, type DialogData } from './dialog';
import { baseColors } from '../lib/customthemes';

function data(over: Partial<DialogData> = {}): DialogData {
  return readData({
    title: 'Run a command of the console',
    message: 'Command: stop\n\nThe console asks to run this command.\nRun it only if you typed it yourself.',
    details: '',
    accept: 'Run',
    cancel: 'Cancel',
    level: 'warning',
    passphrase: false,
    passphraseLabel: '',
    look: { theme: 'midnight' },
    ...over,
  });
}

function key(target: EventTarget, k: string, shift = false) {
  target.dispatchEvent(new KeyboardEvent('keydown', { key: k, shiftKey: shift, bubbles: true, cancelable: true }));
}

beforeEach(() => {
  document.documentElement.removeAttribute('style');
  document.documentElement.removeAttribute('data-theme');
  document.body.replaceChildren();
});

describe('what the host sends', () => {
  it('is read strictly: types, lengths, levels', () => {
    expect(() => readData(null)).toThrow();
    expect(() => readData([1])).toThrow();
    const d = readData({ title: 7, message: 'x'.repeat(7000), accept: '', level: 'fatal', passphrase: 'yes', look: { theme: 3, fontAdjust: 99, fontFamily: 'comic', moneyFont: 'x', lang: '"><b>' } });
    expect(d.title).toBe('');
    expect(d.message.length).toBe(6000);
    expect(d.accept).toBe('OK');
    expect(d.cancel).toBe('Cancel');
    expect(d.level).toBe('warning');
    expect(d.passphrase).toBe(false);
    expect(d.look).toMatchObject({ theme: 'midnight', fontAdjust: 12, fontFamily: '', moneyFont: 'embedded', lang: '', rtl: false });
    expect(readData({ level: 'danger', look: { rtl: true, lang: 'he' } }).look).toMatchObject({ rtl: true, lang: 'he' });
  });
});

describe('the dialog as drawn', () => {
  it('puts every text in as text, never as HTML', () => {
    const r = render(document, data({ title: '<img src=x onerror=alert(1)>', message: '<b>bold</b>\n\n<script>alert(2)</script>', details: '</pre><i>x</i>', accept: '<u>Run</u>' }));
    document.body.append(r.root);
    expect(document.querySelector('img, b, script, i, u')).toBeNull();
    expect(r.root.querySelector('h1')!.textContent).toBe('<img src=x onerror=alert(1)>');
    expect([...r.root.querySelectorAll('.message p')].map((p) => p.textContent)).toEqual(['<b>bold</b>', '<script>alert(2)</script>']);
    expect(r.root.querySelector('#dialog-details')!.textContent).toBe('</pre><i>x</i>');
    expect(r.accept.textContent).toBe('<u>Run</u>');
  });

  it('writes out the marks that turn text around, and isolates each paragraph', () => {
    const r = render(document, data({ title: 'Send to \u202Eexe.txt', message: 'Wallet: a\u2066b\u2069\n\nHost: \u200Fx', details: 'cmd \u202Ddone' }));
    expect(r.root.querySelector('h1')!.textContent).toBe('Send to [U+202E]exe.txt');
    const paragraphs = [...r.root.querySelectorAll('.message p')] as HTMLElement[];
    expect(paragraphs.map((p) => p.textContent)).toEqual(['Wallet: a[U+2066]b[U+2069]', 'Host: [U+200F]x']);
    expect(paragraphs.every((p) => p.dir === 'auto')).toBe(true);
    expect(r.root.querySelector('#dialog-details')!.textContent).toBe('cmd [U+202D]done');
    expect(r.root.textContent).not.toMatch(/[\u202A-\u202E\u2066-\u2069\u200E\u200F\u061C]/);
  });

  it('is an alert dialog, named by its title and described by its text', () => {
    const r = render(document, data());
    expect(r.root.getAttribute('role')).toBe('alertdialog');
    expect(r.root.getAttribute('aria-modal')).toBe('true');
    document.body.append(r.root);
    expect(document.getElementById(r.root.getAttribute('aria-labelledby')!)!.textContent).toBe('Run a command of the console');
    expect(document.getElementById(r.root.getAttribute('aria-describedby')!)!.textContent).toContain('Command: stop');
    expect(document.title).toBe('Run a command of the console');
    // Its line breaks stay (white-space: pre-wrap in dialog.css), its paragraphs apart.
    expect(r.root.querySelectorAll('.message p').length).toBe(2);
  });

  it('marks how grave it is: the accept button of a danger in the colour of errors', () => {
    expect(render(document, data({ level: 'danger' })).accept.className).toContain('danger');
    expect(render(document, data()).accept.className).toContain('primary');
    expect(render(document, data({ level: 'info' })).root.dataset.level).toBe('info');
  });
});

describe('the look of the app', () => {
  const css = readFileSync(resolve(import.meta.dirname, 'dialog.css'), 'utf8');

  it('draws the built-in themes', () => {
    for (const theme of ['midnight', 'light', 'dracula', 'mocha']) {
      expect(applyDialogLook(document, data({ look: { theme } as never }).look, false).choice).toBe(theme);
      expect(document.documentElement.dataset.theme).toBe(theme);
    }
    expect(document.documentElement.style.colorScheme).toBe('dark');
    applyDialogLook(document, data({ look: { theme: 'light' } as never }).look, true);
    expect(document.documentElement.style.colorScheme).toBe('light');
    // "system" follows the desktop.
    applyDialogLook(document, data({ look: { theme: 'system' } as never }).look, true);
    expect(document.documentElement.dataset.theme).toBe('dark');
    // A theme it does not know: the default.
    applyDialogLook(document, data({ look: { theme: 'neon' } as never }).look, false);
    expect(document.documentElement.dataset.theme).toBe('midnight');
  });

  it('draws a theme of the user, its colours set one by one on known tokens', () => {
    const custom = { id: 'custom-mine', name: 'Mine', base: 'dracula', colors: { accent: '#b3125a', bg: '#101010' } };
    const out = applyDialogLook(document, data({ look: { theme: 'custom-mine', custom } as never }).look, false);
    expect(out).toEqual({ choice: 'custom-mine', replaced: false });
    expect(document.documentElement.dataset.theme).toBe('dracula');
    expect(document.documentElement.style.getPropertyValue('--accent')).toBe('#b3125a');
  });

  it('checks a theme of the user as strictly as a theme file: anything else and it is not drawn', () => {
    for (const colors of [{ accent: 'url(https://evil.example/x)' }, { accent: 'red; background: red' }, { nope: '#fff' }]) {
      applyDialogLook(document, data({ look: { theme: 'custom-bad', custom: { id: 'custom-bad', name: 'Bad', base: 'light', colors } } as never }).look, false);
      expect(document.documentElement.dataset.theme).toBe('midnight');
      expect(document.documentElement.getAttribute('style') ?? '').not.toMatch(/url|evil|background/);
    }
  });

  it('draws a theme of the user whose text cannot be read as its base: what is asked is read', () => {
    const custom = { id: 'custom-hide', name: 'Hide', base: 'light', colors: { text: '#ffffff', surface: '#ffffff' } };
    const out = applyDialogLook(document, data({ look: { theme: 'custom-hide', custom } as never }).look, false);
    expect(out).toEqual({ choice: 'light', replaced: true });
    expect(document.documentElement.style.getPropertyValue('--text')).toBe('');
    // Every built-in theme can be read.
    for (const base of ['midnight', 'light', 'dark', 'ocean', 'matrix', 'qdarkstyle', 'mocha', 'dracula'] as const) expect(readable(baseColors(base)), base).toBe(true);
  });

  it('takes the text size, the font and the direction of the language', () => {
    applyDialogLook(document, data({ look: { theme: 'light', fontAdjust: 4, fontFamily: 'nunito', rtl: true, lang: 'ar' } as never }).look, false);
    expect(document.documentElement.style.fontSize).toBe('18px');
    expect(document.documentElement.dataset.font).toBe('nunito');
    expect(document.documentElement.dir).toBe('rtl');
    expect(document.documentElement.lang).toBe('ar');
  });

  it('draws with the colour tokens of the themes only, and the fonts built in', () => {
    const colours = css.replace(/\/\*[\s\S]*?\*\//g, '').match(/#[0-9a-f]{3,8}\b|rgba?\([^)]*\)|hsla?\([^)]*\)/gi) ?? [];
    // Only the glow of Matrix, as base.css has it.
    expect(colours).toEqual(['rgb(0 255 65 / 25%)']);
    expect(css).toMatch(/font-family: var\(--font-ui\)/);
    expect(css).not.toMatch(/url\(/);
  });
});

describe('the keys and the buttons', () => {
  let now = 0;
  const clock = () => now;

  let opened: { dispose(): void }[] = [];

  function open(over: Partial<DialogData> = {}) {
    const r = render(document, data(over));
    document.body.replaceChildren(r.root);
    const answers: { accept: boolean; passphrase?: string }[] = [];
    const control = wire(document, r, (accept, passphrase) => answers.push(passphrase === undefined ? { accept } : { accept, passphrase }), clock);
    opened.push(control);
    control.arm(0);
    return { r, answers, control };
  }

  beforeEach(() => {
    now = 0;
  });

  afterEach(() => {
    for (const c of opened) c.dispose();
    opened = [];
  });

  it('starts on Cancel, the safe default: Enter cancels', () => {
    const { r, answers } = open();
    expect(document.activeElement).toBe(r.cancel);
    now = ARM_MS + 1;
    key(r.cancel, 'Enter');
    expect(answers).toEqual([{ accept: false }]);
  });

  it('Escape cancels, wherever the focus is', () => {
    const { r, answers } = open();
    now = ARM_MS + 1;
    r.accept.focus();
    key(r.accept, 'Escape');
    expect(answers).toEqual([{ accept: false }]);
  });

  it('accepts only on the button that accepts, once, and not right after the window shows', () => {
    const { r, answers, control } = open();
    r.accept.click();
    r.accept.focus();
    key(r.accept, 'Enter');
    expect(answers).toEqual([]);
    now = ARM_MS + 1;
    key(r.accept, 'Enter');
    r.accept.click();
    r.cancel.click();
    expect(answers).toEqual([{ accept: true }]);
    expect(control.answered()).toBe(true);
    expect(r.accept.disabled && r.cancel.disabled).toBe(true);
  });

  it('keeps Tab in the dialog', () => {
    const { r } = open({ passphrase: true, passphraseLabel: 'Passphrase' });
    expect(document.activeElement).toBe(r.field);
    key(r.field!, 'Tab');
    expect(document.activeElement).toBe(r.cancel);
    key(r.cancel, 'Tab');
    expect(document.activeElement).toBe(r.accept);
    key(r.accept, 'Tab');
    expect(document.activeElement).toBe(r.field);
    key(r.field!, 'Tab', true);
    expect(document.activeElement).toBe(r.accept);
  });

  it('a passphrase: needed to accept, given once, the field emptied', () => {
    const { r, answers } = open({ passphrase: true, passphraseLabel: 'Wallet passphrase' });
    expect(r.field!.type).toBe('password');
    expect(r.root.querySelector('label')!.textContent).toContain('Wallet passphrase');
    now = ARM_MS + 1;
    key(r.field!, 'Enter');
    expect(answers).toEqual([]);
    expect(r.field!.getAttribute('aria-invalid')).toBe('true');
    r.field!.value = 'correct horse';
    key(r.field!, 'Enter');
    expect(answers).toEqual([{ accept: true, passphrase: 'correct horse' }]);
    expect(r.field!.value).toBe('');
  });
});

describe('its bridge', () => {
  it('hello, then ready with its size, then the answer, once', async () => {
    document.documentElement.removeAttribute('class');
    const calls: [string, unknown][] = [];
    const bridge = vi.fn(async (method: string, params?: Record<string, unknown>) => {
      calls.push([method, params]);
      if (method === 'hello') return { title: 'Open a web address', message: 'Host: example.org', accept: 'Open', look: { theme: 'light' } };
      return true;
    });
    document.body.className = 'loading';
    const control = await start(bridge, document, window);
    expect(calls.map((c) => c[0])).toEqual(['hello', 'ready']);
    expect(calls[1][1]).toEqual({ width: expect.any(Number), height: expect.any(Number) });
    expect(document.body.classList.contains('loading')).toBe(false);
    expect(document.documentElement.dataset.theme).toBe('light');
    expect(document.getElementById('dialog-title')!.textContent).toBe('Open a web address');
    expect(control.answered()).toBe(false);
    document.getElementById('dialog-cancel')!.click();
    document.getElementById('dialog-accept')!.click();
    expect(calls.map((c) => c[0])).toEqual(['hello', 'ready', 'answer']);
    expect(calls[2][1]).toEqual({ accept: false });
    control.dispose();
  });

  it('never runs text as code, and has no other way to the host', () => {
    const code = readFileSync(resolve(import.meta.dirname, 'dialog.ts'), 'utf8') + readFileSync(resolve(import.meta.dirname, 'main.ts'), 'utf8');
    expect(code).not.toMatch(/innerHTML|outerHTML|insertAdjacentHTML|\beval\(|new Function|__chains\b|__chains\(/);
    expect(code).not.toMatch(/fetch\(|XMLHttpRequest|WebSocket|localStorage|sessionStorage|indexedDB/);
  });
});
