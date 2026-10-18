<script lang="ts">
  // The pages of FastBlocks (chain/local.ts lists them): the one shown. Staking, one section (Finality & BMM and
  // Slashing & jail are views of its Network tab).
  import Staking from './pages/Staking.svelte';
  let { page }: { page: string } = $props();
</script>

{#if page === 'staking'}<Staking />
{/if}
