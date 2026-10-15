<script lang="ts">
  // A validator key: shortened (whole in its tooltip, or whole when asked), with its copy button and, for one of
  // this node's, a badge.
  import CopyButton from '../../components/CopyButton.svelte';
  import { shortKey } from './model';

  let { key, mine = false, whole = false }: { key: string; mine?: boolean; whole?: boolean } = $props();
</script>

<span class="fb-key">
  <span class="mono" class:whole title={key} translate="no">{whole ? key : shortKey(key)}</span>
  <CopyButton text={key} label="Copy the validator key" />
  {#if mine}<span class="badge accent fb-mine">this node</span>{/if}
</span>
