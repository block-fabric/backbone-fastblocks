// The pages of FastBlocks of its own, beside those of the kit (with LocalPages.svelte, which shows them, and
// chain.json): its blocks of stake. Staking (this node as a validator, the wallet's bonds, the validator sets, the
// leader schedule), Finality & BMM (how far the chain is BMM-confirmed and final, the BMM blocks, the fee pot, the
// node's bids) and Slashing & jail (proofs of double signing, jailed and banned validators). Their RPC methods
// and host methods are in src/webui/local.cpp; see doc/webui.md, "Pages of a chain of its own", and
// doc/webui-parity.md, section FB.
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
}

export const LOCAL_PAGES = [
  { id: 'staking', title: 'Staking', icon: 'stake', section: 'chain', shortcut: 'Alt+6', summary: 'This node as a validator, the bonds of the wallet, the validator sets and the leader schedule', keywords: 'fastblocks stake bond unbond validator epoch slot leader schedule jail income pos', quick: { label: 'Stake', icon: 'stake', tab: 'bonds' } },
  { id: 'finality', title: 'Finality & BMM', icon: 'finality', section: 'chain', shortcut: 'Alt+7', summary: 'How far the chain is BMM-confirmed and final, the BMM blocks, the fee pot and the bids', keywords: 'fastblocks bmm anchor acknowledgement ticket final finality confirmed pot bid validatoranchorbid' },
  { id: 'slashing', title: 'Slashing & jail', icon: 'gavel', section: 'chain', shortcut: 'Alt+8', summary: 'Proofs that a leader signed twice, and the validators jailed or banned', keywords: 'fastblocks slash slashing evidence proof double sign jail jailed banned penalty' },
] as const satisfies readonly LocalPage[];

export type LocalPageId = (typeof LOCAL_PAGES)[number]['id'];

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
  { id: 'fb.node', label: 'This node as a validator', page: 'staking', tab: 'node', tip: 'Its keys, staking on or off, the slots it leads next, its income', keywords: 'validator staking income keys' },
  { id: 'fb.validators', label: 'Validator set', page: 'staking', tab: 'validators', tip: 'The validators of an epoch, their stake and share, and every bond on the chain', keywords: 'validators set stake share epoch' },
  { id: 'fb.schedule', label: 'Leader schedule', page: 'staking', tab: 'schedule', tip: 'Who leads each slot of an epoch, and the epochs', keywords: 'slots leaders schedule epochs' },
  { id: 'fb.bmm', label: 'BMM blocks and finality', page: 'finality', tip: 'How far the chain is BMM-confirmed and final, and the newest BMM blocks', keywords: 'bmm anchors final finality pot' },
  { id: 'fb.evidence', label: 'Proofs of double signing', page: 'slashing', tip: 'Proofs that a leader signed two blocks for one slot, and jailed validators', keywords: 'slashing evidence jail banned' },
] as const satisfies readonly LocalCommand[];
