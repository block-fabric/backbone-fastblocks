// The pages of FastBlocks of its own, beside those of the kit (with LocalPages.svelte, which shows them, and
// chain.json).
//
// Staking: the blocks of stake in one section (user decision, 2026-10-18: one section, not three, so as not to
// confuse), with tabs: Overview (the epoch, the slot and its leader, the fee split, your stake and income, your
// coins by finality), Validator (this node as a validator: its keys, staking on or off, its payout and bid, the
// slots it leads), Bonds (bond, add, unbond; the wallet's bonds) and Network (the validator set, the leader
// schedule, finality and the BMM blocks, slashing and jail). Simple mode shows the essentials only (your stake,
// your rewards, bond and unbond: fb/SimpleStaking). The same components as the app of Layers (chain/fb).
//
// Their RPC methods and host methods are in src/webui/local.cpp; see doc/webui.md, "Pages of a chain of its own",
// and doc/webui-parity.md, section FB.
import type { IconName } from '../components/Icon.svelte';

export interface LocalPage {
  /** Stable: the page id (go('names')), the command page.<id>. */
  id: string;
  title: string;
  icon: IconName;
  /** What the page is for, in a line (tooltips, the palette). */
  summary: string;
  /**
   * The section of the sidebars and the area of the workspace it goes in. What concerns the chain comes
   * before the tools, whatever the section (the chainBeforeTools tests).
   */
  section: 'wallet' | 'chain' | 'tools';
  /** The page needs a loaded wallet. */
  wallet?: boolean;
  shortcut?: string;
  /** More words to find it by in the palette. */
  keywords?: string;
  /** A quick action of the dashboard's home that opens it (after Send and Receive, with the chain's). */
  quick?: { label: string; icon?: IconName; tab?: string; simpleLabel?: string };
  /** Its name in Simple mode, in plain words, where it differs from the title ("My bets" for Portfolio). */
  simpleTitle?: string;
  /** Only in the navigation of Advanced mode (lib/uimode.svelte.ts): Simple mode reaches it by the palette and links. */
  advanced?: boolean;
  /**
   * One of the chain's main features, in the navigation of Simple mode (at most three pages; the rule is in
   * doc/webui.md, "Simple and Advanced modes"). It then needs a `simpleSummary`.
   */
  simple?: boolean;
  /** What it is for in Simple mode, in a few plain words (under its name in the sidebar of Simple mode). */
  simpleSummary?: string;
  /**
   * Not a page of its own in the navigation: a tab of another page of the chain (BitAssets: Register new asset
   * under My assets). go('<id>') and its commands lead there (to `tab`, then the tab asked for, "issue/…").
   */
  tabOf?: { page: string; tab: string };
}

export const LOCAL_PAGES = [
  { id: 'staking', title: 'Staking', simpleTitle: 'Stake and earn', simple: true, simpleSummary: 'Earn by putting your coins to work', icon: 'stake', section: 'chain', shortcut: 'Alt+6', summary: 'Blocks of stake: your stake and income, this node as a validator, the bonds, the validators, finality and slashing', keywords: 'fastblocks stake bond unbond validator epoch slot leader schedule jail income pos finality bmm anchor ticket final pot bid slash slashing evidence proof double sign banned penalty fee split', quick: { label: 'Stake', icon: 'stake', tab: 'bonds', simpleLabel: 'Stake and earn' } },
] as const satisfies readonly LocalPage[];

export type LocalPageId = (typeof LOCAL_PAGES)[number]['id'];

/**
 * The tabs of Staking (pages/Staking.svelte), and the views of its Network tab, by the ids deep links give as the
 * tab: go('staking', 'finality') opens Network on Finality & BMM. The ids of the three pages FastBlocks had
 * (finality, slashing) and of its tabs (node, validators, schedule) still lead where they did.
 */
export const STAKING_TABS = ['overview', 'validator', 'bonds', 'network'] as const;
export const NETWORK_VIEWS = ['validators', 'schedule', 'finality', 'slashing'] as const;
export type StakingTab = (typeof STAKING_TABS)[number];
export type NetworkView = (typeof NETWORK_VIEWS)[number];
/** The tab and the view of Network a deep link's tab names (an unknown one: Overview). */
export function stakingTabOf(tab: string | undefined | null): { tab: StakingTab; view: NetworkView } {
  if (tab === 'node') return { tab: 'validator', view: 'validators' };
  if ((NETWORK_VIEWS as readonly string[]).includes(tab ?? '')) return { tab: 'network', view: tab as NetworkView };
  if ((STAKING_TABS as readonly string[]).includes(tab ?? '')) return { tab: tab as StakingTab, view: 'validators' };
  return { tab: 'overview', view: 'validators' };
}

/** Items of the menus (the Menus button and the palette) of this chain: in the group 'Chains' (before the tools). */
export interface LocalCommand {
  id: string;
  label: string;
  page: LocalPageId;
  tab?: string;
  tip?: string;
  keywords?: string;
}

export const LOCAL_COMMANDS = [
  { id: 'fb.bond', label: 'Bond coins…', page: 'staking', tab: 'bonds', tip: 'Bond coins of the wallet to a validator key, add to a bond, or unbond', keywords: 'stake bond unbond validator' },
  { id: 'fb.node', label: 'This node as a validator', page: 'staking', tab: 'validator', tip: 'Its keys, staking on or off, its payout and bid, the slots it leads next', keywords: 'validator staking keys payout' },
  { id: 'fb.validators', label: 'Validator set', page: 'staking', tab: 'validators', tip: 'The validators of an epoch, their stake and share, and every bond on the chain', keywords: 'validators set stake share epoch network' },
  { id: 'fb.schedule', label: 'Leader schedule', page: 'staking', tab: 'schedule', tip: 'Who leads each slot of an epoch, and the epochs', keywords: 'slots leaders schedule epochs' },
  { id: 'fb.bmm', label: 'BMM blocks and finality', page: 'staking', tab: 'finality', tip: 'How far the chain is BMM-confirmed and final, and the newest BMM blocks', keywords: 'bmm anchors final finality pot' },
  { id: 'fb.evidence', label: 'Proofs of double signing', page: 'staking', tab: 'slashing', tip: 'Proofs that a leader signed two blocks for one slot, and jailed validators', keywords: 'slashing evidence jail banned' },
] as const satisfies readonly LocalCommand[];
