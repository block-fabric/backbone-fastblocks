// The first-run tips of Home in Simple mode (components/FirstRunTips.svelte): three steps of this chain,
// each with the page it opens. English here; the card translates them where it shows them.
import type { IconName } from '../components/Icon.svelte';
import type { PageId } from './nav.svelte';

export interface FirstStep {
  title: string;
  text: string;
  icon: IconName;
  /** The button of the step: the page (or Home's side panel) it opens. */
  action?: { label: string; page: PageId; tab?: string; panel?: 'send' | 'receive' };
}

export const FIRST_STEPS_TITLE = 'Welcome to Chains';

export const FIRST_STEPS: readonly FirstStep[] = [
  { title: 'Get your first CHN', text: 'Receive gives you an address. Hand it to whoever pays you; the coins show up here in a minute or so.', icon: 'receive', action: { label: 'Receive', page: 'receive', panel: 'receive' } },
  { title: 'Send a payment', text: 'Send pays an address. The payment is confirmed once it is in a block, about a minute later, and final a few blocks after.', icon: 'send', action: { label: 'Send', page: 'send', panel: 'send' } },
  { title: 'Try a sidechain', text: 'Sidechains add prediction markets, names, private payments and more. Move some CHN to one, and bring it back later from that sidechain’s app.', icon: 'sidechains', action: { label: 'Move coins', page: 'sidechains' } },
];
