<script lang="ts">
  // Advanced transactions (user decision, 2026-10-15): Sign and verify, partially signed transactions
  // (PSBT) and the multisig lounge are the tabs of one page, with the wallet. Their old page ids lead here
  // (PageInfo.tabOf); each keeps its own tabs after "/" in nav.tab ("messages/verify", "psbt/file").
  import { t } from '../lib/i18n.svelte';
  import { nav, outerTab } from '../lib/nav.svelte';
  import Messages from './Messages.svelte';
  import Multisig from './Multisig.svelte';
  import Psbt from './Psbt.svelte';

  const TABS = [
    { id: 'messages', label: 'Sign & verify' },
    { id: 'psbt', label: 'PSBT' },
    { id: 'multisig', label: 'Multisig' },
  ];
  const tab = $derived(TABS.some((tb) => tb.id === outerTab()) ? outerTab() : 'messages');
</script>

<div class="page advanced">
  <header class="page-head"><h1>Advanced transactions</h1></header>
  <div class="tabs" role="tablist" aria-label="Advanced transactions">
    {#each TABS as tb (tb.id)}
      <button type="button" role="tab" aria-selected={tab === tb.id} onclick={() => (nav.tab = tb.id)}>{t(tb.label)}</button>
    {/each}
  </div>
  {#key tab}
    {#if tab === 'messages'}
      <Messages embedded />
    {:else if tab === 'psbt'}
      <Psbt embedded />
    {:else}
      <Multisig embedded />
    {/if}
  {/key}
</div>

<style>
  .advanced {
    display: flex;
    flex-direction: column;
    gap: 0.9rem;
  }
</style>
