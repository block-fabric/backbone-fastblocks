// The RPC methods the pages call are the ones the host lets them call (src/webui/rpcpolicy.cpp, audit M3):
// a method missing there would fail in the app only.
import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join } from 'node:path';
import { describe, expect, it } from 'vitest';

const SRC = join(__dirname, '..');
const POLICY = readFileSync(join(__dirname, '../../../rpcpolicy.cpp'), 'utf8');

function sources(dir: string): string[] {
  return readdirSync(dir).flatMap((name) => {
    const path = join(dir, name);
    if (statSync(path).isDirectory()) return name === 'test' ? [] : sources(path);
    if (!/\.(ts|svelte)$/.test(name) || /\.test\.ts$/.test(name) || name === 'mock.ts') return [];
    return [path];
  });
}

function allowed(): Set<string> {
  const body = POLICY.slice(POLICY.indexOf('methods{'), POLICY.indexOf('return methods;'));
  return new Set([...body.matchAll(/\{"([a-z0-9_]+)", (?:WORKER|LONG)\}/g)].map((m) => m[1]));
}

function keyed(): Set<string> {
  const fn = POLICY.slice(POLICY.indexOf('bool KeyedWalletRpc'));
  const list = fn.slice(fn.indexOf('methods{'), fn.indexOf('};'));
  return new Set([...list.matchAll(/"([a-z0-9_]+)"/g)].map((m) => m[1]));
}

/** The methods named in a call of the RPC client, of walletRpc, or of withKeys. */
function used(): { rpc: Map<string, string>; keyed: Map<string, string>; dynamic: string[] } {
  const rpc = new Map<string, string>();
  const keyedCalls = new Map<string, string>();
  const dynamic: string[] = [];
  for (const file of sources(SRC)) {
    const text = readFileSync(file, 'utf8');
    const where = file.slice(SRC.length + 1);
    for (const m of text.matchAll(/rpc\.(?:raw|call)\b[\s\S]{0,200}?\(\s*(['"`]?)([A-Za-z0-9_.]+)/g)) {
      if (m[1]) rpc.set(m[2], where);
      else dynamic.push(`${where}: ${m[2]}`);
    }
    for (const m of text.matchAll(/walletRpc\b[^(]*\([^,]+,\s*'([a-z0-9_]+)'/g)) {
      rpc.set(m[1], where);
      keyedCalls.set(m[1], where);
    }
    if (/withKeys</.test(text) || /withKeys\(/.test(text)) {
      for (const m of text.matchAll(/method:\s*'([a-z0-9_]+)'/g)) keyedCalls.set(m[1], where);
    }
    // The Sidechains page runs its RPC methods through a helper: call('method', [...]).
    if (where.endsWith('Sidechains.svelte')) for (const m of text.matchAll(/\bcall\(\s*'([a-z0-9_]+)'/g)) rpc.set(m[1], where);
  }
  return { rpc, keyed: keyedCalls, dynamic };
}

describe('the RPC methods of the pages', () => {
  it('are all allowed by the host', () => {
    const ok = allowed();
    expect(ok.size).toBeGreaterThan(30);
    const { rpc } = used();
    expect(rpc.size).toBeGreaterThan(30);
    const missing = [...rpc].filter(([m]) => !ok.has(m)).map(([m, where]) => `${m} (${where})`);
    expect(missing).toEqual([]);
  });

  it('that need keys are the ones the host runs with keys', () => {
    const ok = keyed();
    const { keyed: calls } = used();
    expect(calls.size).toBeGreaterThan(0);
    const missing = [...calls].filter(([m]) => !ok.has(m)).map(([m, where]) => `${m} (${where})`);
    expect(missing).toEqual([]);
  });

  it('are named in the code, but for the two helpers that pass one on', () => {
    // A method chosen at run time could be one the host refuses: only these helpers pass one on.
    expect(used().dynamic.sort()).toEqual(['lib/dialogs.svelte.ts: method', 'pages/Sidechains.svelte: method']);
  });

  it('do not include what writes files or stops the node', () => {
    const ok = allowed();
    for (const m of ['backupwallet', 'dumptxoutset', 'dumpwallet', 'stop', 'importdescriptors', 'invalidateblock', 'setban', 'addnode']) expect(ok.has(m)).toBe(false);
  });
});
