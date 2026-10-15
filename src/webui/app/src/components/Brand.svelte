<script lang="ts">
  // The mark of Chains, its name and the network (testnet, signet, regtest).
  import { app } from '../lib/state.svelte';

  let {
    name = true,
    onclick,
    label = 'Chains: home',
    expanded,
  }: { name?: boolean; onclick?: () => void; label?: string; expanded?: boolean } = $props();
</script>

{#snippet content()}
  <svg class="mark" viewBox="0 0 32 32" aria-hidden="true">
    <rect x="3" y="9" width="12" height="14" rx="3.5" />
    <rect x="17" y="9" width="12" height="14" rx="3.5" />
    <path d="M11 16h10" />
  </svg>
  {#if name}
    <span class="name">Chains</span>
    {#if app.info?.networkLabel}<span class="badge warning net">{app.info.networkLabel}</span>{/if}
  {/if}
{/snippet}

{#if onclick}
  <button type="button" class="brand" {onclick} aria-label={label} title={label} aria-expanded={expanded}>{@render content()}</button>
{:else}
  <div class="brand">{@render content()}</div>
{/if}

<style>
  .brand {
    display: flex;
    align-items: center;
    gap: 0.55rem;
    padding: 0.2rem 0.4rem;
    border: 0;
    border-radius: var(--radius-s);
    background: none;
    color: var(--text);
    font: inherit;
  }
  button.brand {
    cursor: pointer;
  }
  button.brand:hover {
    background: var(--raised);
  }
  .mark {
    width: 1.9rem;
    height: 1.9rem;
    flex: none;
    fill: none;
    stroke: var(--accent);
    stroke-width: 2.4;
  }
  .name {
    font-size: 1.15rem;
    font-weight: 700;
    letter-spacing: -0.02em;
  }
</style>
