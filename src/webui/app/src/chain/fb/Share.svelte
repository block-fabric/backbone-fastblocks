<script lang="ts">
  // The share of a stake in its set: the figure, and a thin bar.
  import { css } from '../../lib/actions';
  import { formatShare } from './model';

  let { percent }: { percent: number } = $props();
</script>

<span class="fb-share" title="The share of the set's stake: of the slots to expect">
  <span class="num">{formatShare(percent)}</span>
  <span class="bar" aria-hidden="true"><span use:css={{ width: `${Math.min(100, Math.max(0, percent))}%` }}></span></span>
</span>
