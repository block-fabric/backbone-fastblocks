<script lang="ts">
  // The pages of FastBlocks (chain/local.ts lists them): the one shown.
  import Finality from './pages/Finality.svelte';
  import Slashing from './pages/Slashing.svelte';
  import Staking from './pages/Staking.svelte';
  let { page }: { page: string } = $props();
</script>

{#if page === 'staking'}<Staking />
{:else if page === 'finality'}<Finality />
{:else if page === 'slashing'}<Slashing />
{/if}
