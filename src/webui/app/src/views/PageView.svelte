<script lang="ts">
  // The page shown, whatever the layout around it.
  import NoWallet from '../components/NoWallet.svelte';
  import Explorer from '../pages/Explorer.svelte';
  import Kit from '../pages/Kit.svelte';
  import Mine from '../pages/Mine.svelte';
  import NodeWindow from '../pages/NodeWindow.svelte';
  import Overview from '../pages/Overview.svelte';
  import Advanced from '../pages/Advanced.svelte';
  import Tools from '../pages/Tools.svelte';
  import Receive from '../pages/Receive.svelte';
  import Send from '../pages/Send.svelte';
  import Settings from '../pages/Settings.svelte';
  import SidechainNodes from '../pages/SidechainNodes.svelte';
  import Sidechains from '../pages/Sidechains.svelte';
  import Transactions from '../pages/Transactions.svelte';
  import Wallets from '../pages/Wallets.svelte';
  import Home from './home/Home.svelte';
  import { nav, PAGES } from '../lib/nav.svelte';
  import { wallets, walletsEnabled } from '../lib/state.svelte';

  const noWallet = $derived(wallets.list.length === 0);
  // With -disablewallet there are no wallet pages (chains-qt builds no wallet UI then): they say why.
  const walletsOn = $derived(walletsEnabled());
  const page = $derived(PAGES[nav.page]);
</script>

{#key nav.page}
  {#if nav.page === 'home'}
    <Home />
  {:else if nav.page === 'overview'}
    <Overview />
  {:else if nav.page === 'node'}
    <NodeWindow />
  {:else if nav.page === 'settings'}
    <Settings />
  {:else if nav.page === 'kit'}
    <Kit />
  {:else if nav.page === 'sidechains'}
    <Sidechains />
  {:else if nav.page === 'sidechain-nodes'}
    <SidechainNodes />
  {:else if nav.page === 'mine'}
    <Mine />
  {:else if nav.page === 'explorer'}
    <Explorer />
  {:else if nav.page === 'tools'}
    <Tools />
  {:else if (page.wallet && noWallet) || (!walletsOn && nav.page === 'wallets')}
    <NoWallet {page} />
  {:else if nav.page === 'send'}
    <Send />
  {:else if nav.page === 'receive'}
    <Receive />
  {:else if nav.page === 'transactions'}
    <Transactions />
  {:else if nav.page === 'wallets'}
    <Wallets />
  {:else if nav.page === 'advanced'}
    <Advanced />
  {/if}
{/key}
