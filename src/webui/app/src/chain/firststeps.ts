// The three first steps of FastBlocks, shown on Home in Simple mode until dismissed
// (components/FirstRunTips.svelte; lib/firststeps.ts). English here: the card translates them.
import type { FirstStep } from '../lib/firststeps';
import { CHAIN } from './config';

const main = CHAIN.mainchain?.name ?? 'the mainchain';

export const FIRST_STEPS: readonly FirstStep[] = [
  { title: 'Bring coins here', text: `Coins come from ${main}. Drivechains gives a deposit address: paste it in the ${main} app (Sidechains, Move coins). They arrive here a block or two later.`, icon: 'down', action: { label: 'Get a deposit address', page: 'peg', tab: 'deposit' } },
  { title: 'Pay in seconds', text: 'A block every 15 seconds: payments here are confirmed almost at once, and final once the miners of the main chain have seen them.', icon: 'send', action: { label: 'Send', page: 'send', panel: 'send' } },
  { title: 'Stake and earn', text: 'Lock some coins so that your node takes turns making blocks, and earn part of their fees. Stop whenever you like.', icon: 'stake', action: { label: 'Stake and earn', page: 'staking' } },
];
