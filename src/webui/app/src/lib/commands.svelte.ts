// Every action of the main window in one list: the pages, and the items of the menu bar of
// chains-qt (bitcoingui.cpp createMenuBar), which the layouts put in tabs, sidebars, an
// overflow menu and the command palette (doc/webui-layouts.md maps each one).
import { t } from './i18n.svelte';
import type { IconName } from '../components/Icon.svelte';
import { closeDetail } from './detail.svelte';
import { LAYOUTS, pageIcon, type LayoutId } from './layout';
import { go, nav, PAGES, type PageId } from './nav.svelte';
import { host } from './rpc';
import { app, node, prefs, savePrefs, toast, wallets, walletsEnabled } from './state.svelte';
import { clampFontAdjust, THEMES } from './themes';
import { UNITS } from './units';

/** The menus of chains-qt, in their order, then the groups of the app. */
// The menus: what concerns the chain (sidechains, mining, the explorer) before the tools.
export const MENU_GROUPS = ['Your wallet', 'Banking', 'Chains', 'Tools', 'Window', 'This node'] as const;
export type MenuGroup = (typeof MENU_GROUPS)[number];
export type CommandGroup = 'Go to' | MenuGroup | 'Layout' | 'Display' | 'Network';

export interface Command {
  /** Stable: tests and the mapping of doc/webui-layouts.md use it. */
  id: string;
  label: string;
  group: CommandGroup;
  run: () => void;
  icon?: IconName;
  shortcut?: string;
  disabled?: boolean;
  /** A setting that is on or off (shown with a mark). */
  checked?: boolean;
  /** More words to find it by. */
  keywords?: string;
  /** In a menu: a line above it. */
  separator?: boolean;
  /** The status tip of chains-qt (QAction::statusTip): shown in the status bar under the pointer. */
  tip?: string;
  /** The page it opens, if it opens one (what decides where it goes: chain before tools). */
  page?: PageId;
}

export const palette = $state({ open: false });

export function openPalette(): void {
  palette.open = true;
}

/** Switch the layout; the home page belongs to the dashboard. */
export function setLayout(id: LayoutId): void {
  if (prefs.layout === id) return;
  closeDetail();
  savePrefs({ layout: id });
  if (id === 'dashboard' && nav.page === 'overview') go('home');
  else if (id !== 'dashboard' && nav.page === 'home') go('overview');
}

/** The pages that can be shown now (the kit only for tests and developers, home only on the dashboard). */
export function pageAvailable(id: PageId): boolean {
  // With -disablewallet there are no wallet pages (chains-qt builds no wallet UI then).
  if ((PAGES[id].wallet || id === 'wallets') && !walletsEnabled()) return false;
  if (id === 'kit') return !!(app.info?.test || app.info?.devtools);
  if (id === 'home') return prefs.layout === 'dashboard';
  // A tab of another page: reached through it (and through its own command), not as a page.
  if (PAGES[id].tabOf) return false;
  return true;
}

/** A page that needs a wallet, without one (or the history in privacy mode). */
export function pageDisabled(id: PageId): boolean {
  if (PAGES[id].wallet && wallets.list.length === 0) return true;
  return id === 'transactions' && prefs.mask;
}

/** All the commands, from the state now (call it in a $derived). */
export function buildCommands(): Command[] {
  const noWallet = wallets.list.length === 0;
  const currentWallet = wallets.list.find((w) => w.name === wallets.current) ?? null;
  const encrypted = !!currentWallet?.encrypted;
  const watchOnly = !!currentWallet?.privateKeysDisabled;
  const to = (page: PageId, tab = '') => Object.assign(() => go(page, tab), { page });
  const font = (delta: number) => () => savePrefs({ fontAdjust: clampFontAdjust(prefs.fontAdjust + delta) });

  const pages: Command[] = (Object.keys(PAGES) as PageId[]).filter(pageAvailable).map((id) => ({
    id: `page.${id}`,
    label: PAGES[id].title,
    group: 'Go to',
    icon: pageIcon(id),
    shortcut: PAGES[id].shortcut,
    disabled: pageDisabled(id),
    keywords: PAGES[id].summary,
    run: to(id),
  }));

  const menus: Command[] = [
    // Your wallet
    { id: 'wallet.create', group: 'Your wallet', label: 'Create wallet…', run: to('wallets', 'create'), keywords: 'new', tip: 'Create a new wallet' },
    { id: 'wallet.open', group: 'Your wallet', label: 'Open wallet…', run: to('wallets', 'open'), keywords: 'load', tip: 'Open a wallet' },
    { id: 'wallet.close', group: 'Your wallet', label: 'Close wallet…', run: to('wallets', 'close'), disabled: noWallet, keywords: 'unload', tip: 'Close wallet' },
    { id: 'wallet.closeAll', group: 'Your wallet', label: 'Close all wallets…', run: to('wallets', 'close-all'), disabled: noWallet, tip: 'Close all wallets' },
    { id: 'wallet.migrate', group: 'Your wallet', label: 'Migrate wallet…', run: to('wallets', 'migrate'), keywords: 'legacy descriptor', tip: 'Migrate a wallet' },
    { id: 'wallet.restoreMigrate', group: 'Your wallet', label: 'Restore and migrate wallet file…', run: to('wallets', 'restore-migrate'), keywords: 'legacy' },
    // No sending addresses (user decision, 2026-10-15): the receiving ones are a tab of Receive.
    { id: 'wallet.receivingAddresses', group: 'Your wallet', label: 'Receiving addresses', run: to('receive', 'addresses'), disabled: noWallet, separator: true, keywords: 'address book labels generate new', tip: 'Show the list of used receiving addresses and labels' },
    { id: 'wallet.backup', group: 'Your wallet', label: 'Backup wallet…', run: to('wallets', 'backup'), disabled: noWallet, separator: true, keywords: 'save copy', tip: 'Backup wallet to another location' },
    { id: 'wallet.restore', group: 'Your wallet', label: 'Restore wallet…', run: to('wallets', 'restore'), keywords: 'backup file', tip: 'Restore a wallet from a backup file' },
    { id: 'wallet.exportWatchOnly', group: 'Your wallet', label: 'Export watch-only wallet…', run: to('wallets', 'export'), disabled: noWallet || watchOnly, tip: 'Export a watch-only version of the current wallet that can be restored onto another node.' },
    { id: 'wallet.encrypt', group: 'Your wallet', label: 'Encrypt wallet…', run: to('wallets', 'encrypt'), disabled: noWallet || encrypted || watchOnly, separator: true, keywords: 'passphrase password', tip: 'Encrypt the private keys that belong to your wallet' },
    { id: 'wallet.passphrase', group: 'Your wallet', label: 'Change passphrase…', run: to('wallets', 'passphrase'), disabled: noWallet || !encrypted, keywords: 'password', tip: 'Change the passphrase used for wallet encryption' },
    { id: 'wallet.unlock', group: 'Your wallet', label: 'Unlock wallet for a while…', run: to('wallets', 'unlock'), disabled: noWallet || !encrypted, keywords: 'passphrase' },
    { id: 'wallet.mask', group: 'Your wallet', label: 'Mask values', run: () => savePrefs({ mask: !prefs.mask }), checked: prefs.mask, shortcut: 'Ctrl+Shift+M', keywords: 'privacy hide amounts', tip: 'Mask the values in the Overview tab' },
    // Banking
    { id: 'banking.send', group: 'Banking', label: 'Send', run: to('send'), shortcut: 'Alt+2', disabled: noWallet, keywords: 'pay', tip: 'Send coins to a Chains address' },
    { id: 'banking.receive', group: 'Banking', label: 'Receive', run: to('receive'), shortcut: 'Alt+3', disabled: noWallet, keywords: 'request address qr', tip: 'Request payments (generates QR codes and chains: URIs)' },
    { id: 'banking.transactions', group: 'Banking', label: 'Transactions', run: to('transactions'), shortcut: 'Alt+4', disabled: noWallet || prefs.mask, keywords: 'history', tip: 'Browse transaction history' },
    { id: 'banking.openUri', group: 'Banking', label: 'Open URI…', run: to('send', 'uri'), disabled: noWallet, keywords: 'payment link chains:', tip: 'Open a chains: URI' },
    { id: 'banking.proof', group: 'Banking', label: 'Proof of funds', run: to('proof'), disabled: noWallet, separator: true },
    { id: 'banking.multisig', group: 'Banking', label: 'Multisig lounge', run: to('multisig'), disabled: noWallet },
    { id: 'banking.psbtFile', group: 'Banking', label: 'Load PSBT from file…', run: to('psbt', 'file'), disabled: noWallet, separator: true, keywords: 'partially signed', tip: 'Load Partially Signed Chains Transaction' },
    { id: 'banking.psbtClipboard', group: 'Banking', label: 'Load PSBT from clipboard…', run: to('psbt', 'clipboard'), disabled: noWallet, keywords: 'partially signed paste', tip: 'Load Partially Signed Chains Transaction from clipboard' },
    // Chains: what concerns the chain, before the tools
    { id: 'use.sidechains', group: 'Chains', label: 'Sidechains', run: to('sidechains'), shortcut: 'Alt+5', keywords: 'deposit', tip: 'Deposit to sidechains, and manage sidechain proposals and withdrawal votes' },
    { id: 'chains.nodes', group: 'Chains', label: 'Sidechain nodes', run: to('sidechain-nodes'), keywords: 'bmm merged mining start', tip: 'Install, start and stop the nodes of sidechains, and watch them' },
    { id: 'chains.mine', group: 'Chains', label: 'Solo mine', run: to('mine'), tip: 'Mine blocks with the processor of this computer' },
    { id: 'chains.proposals', group: 'Chains', label: 'Sidechain proposals', run: to('sidechains', 'proposals'), keywords: 'ack acknowledge vote', tip: 'Propose sidechains, acknowledge proposals and vote on withdrawals in the blocks this node mines' },
    { id: 'chains.withdrawals', group: 'Chains', label: 'Withdrawal votes', run: to('sidechains', 'withdrawals'), keywords: 'bundle withdrawal vote upvote' },
    { id: 'tools.explorer', group: 'Chains', label: 'Block explorer', run: to('explorer'), keywords: 'blocks search' },
    // Tools
    { id: 'use.timestamp', group: 'Tools', label: 'Timestamp file', run: to('timestamp'), keywords: 'proof of existence' },
    { id: 'use.sign', group: 'Tools', label: 'Sign message…', run: to('messages', 'sign'), disabled: noWallet, separator: true, tip: 'Sign messages with your Chains addresses to prove you own them' },
    { id: 'use.verify', group: 'Tools', label: 'Verify message…', run: to('messages', 'verify'), disabled: noWallet, tip: 'Verify messages to ensure they were signed with specified Chains addresses' },
    { id: 'tools.hash', group: 'Tools', label: 'Hash calculator', run: to('crypto', 'hash'), keywords: 'sha256' },
    { id: 'tools.merkle', group: 'Tools', label: 'Merkle tree', run: to('crypto', 'merkle') },
    { id: 'tools.address', group: 'Tools', label: 'Address decoder', run: to('crypto', 'address') },
    // Window
    { id: 'window.minimize', group: 'Window', label: 'Minimize', run: () => void host.minimize().catch(() => undefined), shortcut: 'Ctrl+M' },
    { id: 'window.info', group: 'Window', label: 'Information', run: to('node', 'info'), shortcut: 'Ctrl+I', separator: true, keywords: 'node window' },
    { id: 'window.console', group: 'Window', label: 'Console', run: to('node', 'console'), shortcut: 'Ctrl+T', keywords: 'rpc command' },
    { id: 'window.traffic', group: 'Window', label: 'Network traffic', run: to('node', 'traffic'), shortcut: 'Ctrl+N' },
    { id: 'window.peers', group: 'Window', label: 'Peers', run: to('node', 'peers'), shortcut: 'Ctrl+P', keywords: 'connections ban' },
    // This node
    { id: 'node.window', group: 'This node', label: 'Node window', run: to('node'), shortcut: 'Ctrl+Shift+D', tip: 'Open node debugging and diagnostic console' },
    { id: 'node.options', group: 'This node', label: 'Options…', run: to('settings', 'main'), keywords: 'settings preferences', tip: 'Modify configuration options for Chains' },
    { id: 'node.sync', group: 'This node', label: 'Sync status…', run: () => (nav.dialog = 'sync'), keywords: 'synchronizing progress blocks left' },
    ...THEMES.map((t): Command => ({ id: `node.theme.${t.id}`, group: 'This node', label: `Theme: ${t.name}`, run: () => savePrefs({ theme: t.id }), checked: prefs.theme === t.id, keywords: 'look colours' })),
    { id: 'node.themeCustom', group: 'This node', label: 'Theme: Custom…', run: to('settings', 'display'), keywords: 'look colours font' },
    { id: 'node.fontUp', group: 'This node', label: 'Increase font size', run: font(1), shortcut: 'Ctrl++', keywords: 'text larger zoom' },
    { id: 'node.fontDown', group: 'This node', label: 'Decrease font size', run: font(-1), shortcut: 'Ctrl+−', keywords: 'text smaller zoom' },
    { id: 'node.help', group: 'This node', label: 'Command-line options', run: () => (nav.dialog = 'help'), keywords: 'help arguments', tip: 'Show the Chains help message to get a list with possible Chains command-line options' },
    { id: 'node.about', group: 'This node', label: `About ${app.info?.name ?? 'Chains'}`, run: () => (nav.dialog = 'about'), separator: true, keywords: 'version licence', tip: `Show information about ${app.info?.name ?? 'Chains'}` },
    ...(pageAvailable('kit') ? [{ id: 'node.kit', group: 'This node', label: 'Interface kit', run: to('kit') } satisfies Command] : []),
    { id: 'node.exit', group: 'This node', label: 'Exit', run: () => void host.quit().catch(() => undefined), shortcut: 'Ctrl+Q', separator: true, keywords: 'quit shut down', tip: 'Quit application' },
  ];

  const extra: Command[] = [
    ...LAYOUTS.map((l): Command => ({ id: `layout.${l.id}`, group: 'Layout', label: `Layout: ${l.name}`, icon: l.icon, run: () => setLayout(l.id), checked: prefs.layout === l.id, keywords: l.summary })),
    ...(prefs.layout === 'sidebar'
      ? [{ id: 'layout.sidebarNames', group: 'Layout', label: 'Show the names of the pages in the sidebar', run: () => savePrefs({ sidebarExpanded: !prefs.sidebarExpanded }), checked: prefs.sidebarExpanded, keywords: 'expand collapse icons' } satisfies Command]
      : []),
    ...UNITS.map((u): Command => ({ id: `display.unit.${u.id}`, group: 'Display', label: `Show amounts in ${u.short}`, run: () => savePrefs({ unit: u.id }), checked: prefs.unit === u.id, keywords: `unit ${u.long}` })),
    { id: 'network.peers', group: 'Network', label: 'Show peers', run: to('node', 'peers'), keywords: 'connections' },
    {
      id: 'network.toggle',
      group: 'Network',
      label: node.networkActive ? 'Disable network activity' : 'Enable network activity',
      keywords: 'offline online connections',
      run: () =>
        void host.setNetworkActive(!node.networkActive).catch((e) => toast('error', t('Could not change network activity'), e instanceof Error ? e.message : String(e))),
    },
  ];

  // With -disablewallet: no wallet menus, nor the items that need a wallet (chains-qt builds no wallet UI).
  const walletOnly = new Set(['Your wallet', 'Banking']);
  const shown = walletsEnabled() ? menus : menus.filter((c) => !walletOnly.has(c.group) && c.id !== 'use.sign' && c.id !== 'use.verify');
  // What a command opens, from the function it runs (to(page)).
  return [...pages, ...shown, ...extra].map((c) => ('page' in c.run ? { ...c, page: (c.run as { page: PageId }).page } : c));
}

/** The commands of the old menu bar, by menu (the overflow menus of the layouts). */
export function menuSections(all: readonly Command[]): { title: MenuGroup; items: Command[] }[] {
  // A menu with nothing in it (the wallet menus with -disablewallet) is not shown.
  return MENU_GROUPS.map((title) => ({ title, items: all.filter((c) => c.group === title) })).filter((m) => m.items.length > 0);
}

function norm(text: string): string {
  return text
    .toLowerCase()
    .normalize('NFKD')
    .replace(/[̀-ͯ]/g, '')
    .replace(/[…:]/g, '');
}

/**
 * The commands matching what is typed: every word must be found in the name, the group or the
 * keywords; names that start with the words come first, then names that hold them.
 */
export function searchCommands(all: readonly Command[], query: string): Command[] {
  const words = norm(query).split(/\s+/).filter(Boolean);
  if (words.length === 0) return [...all];
  const scored: { c: Command; score: number; i: number }[] = [];
  all.forEach((c, i) => {
    const label = norm(c.label);
    const hay = `${label} ${norm(c.group)} ${norm(c.keywords ?? '')}`;
    if (!words.every((w) => hay.includes(w))) return;
    let score = 0;
    if (label.startsWith(words[0])) score += 4;
    if (words.every((w) => label.includes(w))) score += 2;
    if (label.split(/\s+/).some((part) => part.startsWith(words[0]))) score += 1;
    if (c.disabled) score -= 3;
    scored.push({ c, score, i });
  });
  scored.sort((a, b) => b.score - a.score || a.i - b.i);
  return scored.map((s) => s.c);
}
