<script lang="ts">
  // Tools (user decision, 2026-10-15): Timestamp a file, Proof of funds and the crypto tools (hash
  // calculator, Merkle tree, address decoder) are the tabs of one page. Their old page ids lead here
  // (PageInfo.tabOf); each keeps its own tabs after "/" in nav.tab ("crypto/merkle").
  import { t } from '../lib/i18n.svelte';
  import NoWallet from '../components/NoWallet.svelte';
  import { nav, outerTab, PAGES } from '../lib/nav.svelte';
  import { wallets, walletsEnabled } from '../lib/state.svelte';
  import Crypto from './Crypto.svelte';
  import Proof from './Proof.svelte';
  import Timestamp from './Timestamp.svelte';

  const TABS = [
    { id: 'timestamp', label: 'Timestamp' },
    { id: 'proof', label: 'Proof of funds' },
    { id: 'crypto', label: 'Crypto tools' },
  ];
  const tab = $derived(TABS.some((tb) => tb.id === outerTab()) ? outerTab() : 'timestamp');
</script>

<div class="page tools">
  <header class="page-head"><h1>Tools</h1></header>
  <div class="tabs" role="tablist" aria-label="Tools">
    {#each TABS as tb (tb.id)}
      <button type="button" role="tab" aria-selected={tab === tb.id} onclick={() => (nav.tab = tb.id)}>{t(tb.label)}</button>
    {/each}
  </div>
  {#key tab}
    {#if tab === 'timestamp'}
      <Timestamp embedded />
    {:else if tab === 'proof'}
      <!-- The proof is made with a wallet: without one (or with -disablewallet), the tab says why. -->
      {#if !walletsEnabled() || wallets.list.length === 0}<NoWallet page={PAGES.proof} />{:else}<Proof embedded />{/if}
    {:else}
      <Crypto embedded />
    {/if}
  {/key}
</div>

<style>
  .tools {
    display: flex;
    flex-direction: column;
    gap: 0.9rem;
  }
</style>
