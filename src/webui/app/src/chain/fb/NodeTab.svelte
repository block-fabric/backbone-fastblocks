<script lang="ts">
  // Staking, This node: the node as a validator (its keys and their state, staking on or off, where its share of
  // fees goes, what it made and missed), the slots it leads next, the staking income of the wallet and its coins
  // by finality.
  import Amount from '../../components/Amount.svelte';
  import CopyButton from '../../components/CopyButton.svelte';
  import Icon from '../../components/Icon.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import { request } from '../../lib/bridge';
  import { errorText, inform } from '../../lib/dialogs.svelte';
  import { formatCount, formatTime } from '../../lib/format';
  import { t, tn } from '../../lib/i18n.svelte';
  import { go } from '../../lib/nav.svelte';
  import { wallets } from '../../lib/state.svelte';
  import { coinsToSats } from '../../lib/units';
  import { css } from '../../lib/actions';
  import { clock } from './clock.svelte';
  import Key from './Key.svelte';
  import { incomeTotal, slotsLed, stateText, stateTip, stateTone, untilText, validatorState, type Income } from './model';
  import { fb, refreshFb } from './store.svelte';

  let { income }: { income: Income | null } = $props();

  const pos = $derived(fb.pos);
  const staking = $derived(fb.staking);
  const localKeys = $derived(new Set(staking?.keys.map((k) => k.pubkey) ?? []));
  const NEXT_SHOWN = 8;
  const next = $derived(pos ? slotsLed(fb.schedules, localKeys, pos.slot + 1, NEXT_SHOWN) : []);
  // The timeline: the slots of the current epoch and the next, this node's lit.
  const timeline = $derived(fb.schedules.flatMap((s) => s.slots));

  let switching = $state(false);
  async function setStaking(enabled: boolean) {
    switching = true;
    try {
      await request('host', 'fastblocks.staking', { enabled });
      await refreshFb();
    } catch (e) {
      // A no on the desktop changes nothing: no need to say it.
      if ((e as { code?: number }).code !== -32800) await inform(t('Staking'), errorText(e), 'warning');
    } finally {
      switching = false;
    }
  }

  const finality = $derived(fb.finality);
  const finalityTotal = $derived(finality ? coinsToSats(finality.soft) + coinsToSats(finality.anchored) + coinsToSats(finality.final) : 0n);
  const part = (coins: number) => (finalityTotal > 0n ? Number((coinsToSats(coins) * 10000n) / finalityTotal) / 100 : 0);
</script>

<div class="side-cols">
  <section class="panel" aria-labelledby="fb-validator-title">
    <div class="panel-head">
      <h2 id="fb-validator-title">This node as a validator</h2>
      {#if staking?.validator}
        {#if staking.enabled}
          <button type="button" class="btn small" id="fb-staking-stop" disabled={switching} onclick={() => setStaking(false)} title={t('Make no blocks of stake until started again (a validator that misses its slots may be jailed)')}><Icon name="power" size={14} /> Stop staking</button>
        {:else}
          <button type="button" class="btn small primary" id="fb-staking-start" disabled={switching} onclick={() => setStaking(true)} title={t("Make blocks of stake at the slots this node's validator keys lead")}><Icon name="power" size={14} /> Start staking</button>
        {/if}
      {/if}
    </div>
    <div class="panel-body side-form">
      {#if !staking?.validator}
        <p class="side-note">
          This node has no validator keys, so it makes no blocks of stake. A validator runs its node with <span class="mono">-validatorkeyfile=&lt;file&gt;</span> (32-byte secret keys in hex, one per line), and bonds coins to its key from a wallet (Bonds).
        </p>
        {#if staking?.producererror}<p class="side-note side-bad">The block producer could not start: <SafeText text={staking.producererror} /></p>{/if}
      {:else}
        <ul class="fb-next" aria-label="Validator keys">
          {#each staking.keys as k (k.pubkey)}
            {@const st = validatorState(k.bond, k.inset, pos?.epoch ?? 0)}
            <li>
              <Key key={k.pubkey} />
              <span class="status {stateTone(st)}" title={stateTip(st)}>{stateText(st, k.bond)}</span>
            </li>
          {/each}
        </ul>
        {#if staking.payout}
          <div class="field">
            <span class="label" title={t("Where the producer's share of the fees of this node's blocks of stake goes: -validatorpayout, else an address of the node's wallet, else the first validator key's own address")}>Fees paid to</span>
            <div class="side-code"><span class="mono" translate="no">{staking.payout}</span><CopyButton text={staking.payout} label="Copy the address" /></div>
          </div>
        {/if}
        <dl class="fb-pairs">
          <dt>Blocks made today</dt><dd class="num">{formatCount(staking.blocksmadesince ?? 0)}</dd>
          <dt>Blocks made on the active chain</dt><dd class="num">{formatCount(staking.blocksmade)}</dd>
          <dt>Since the node started</dt><dd class="num">{tn('%n block(s), %1 of them BMM blocks', staking.produced, formatCount(staking.acknowledgements))}</dd>
          <dt title={t('Slots its keys led and it made no block for, since it started. A validator that misses too many in an epoch is jailed for a while.')}>Missed slots</dt>
          <dd class="num" class:side-warn={staking.failed > 0}>{formatCount(staking.failed)}</dd>
          <dt>Last block made</dt><dd class="num">{staking.lastslot >= 0 ? t('slot %1', formatCount(staking.lastslot)) : t('none yet')}</dd>
          <dt title={t('The slashing protection never signs at or below this slot again.')}>Highest slot signed</dt>
          <dd class="num">{staking.lastsignedslot >= 0 ? formatCount(staking.lastsignedslot) : '–'}</dd>
        </dl>
        {#if staking.error}<p class="side-note side-warn">Last problem: <SafeText text={staking.error} /></p>{/if}
      {/if}
    </div>
  </section>

  <section class="panel" aria-labelledby="fb-next-title">
    <div class="panel-head">
      <h2 id="fb-next-title">Slots it leads next</h2>
      {#if pos}<span class="muted fb-small">{t('epoch %1 and the next', formatCount(pos.epoch))}</span>{/if}
    </div>
    <div class="panel-body">
      {#if timeline.length}
        <div class="fb-slots" role="img" aria-label={t('The slots of this epoch and the next: this node leads %1 of them', timeline.filter((s) => s.leader && localKeys.has(s.leader)).length)}>
          {#each timeline as s (s.slot)}
            <span class:mine={!!s.leader && localKeys.has(s.leader)} class:empty={!s.leader} class:now={s.slot === pos?.slot} title={`${t('Slot %1', formatCount(s.slot))} · ${formatTime(s.time)}`}></span>
          {/each}
        </div>
        <p class="fb-legend"><span><i class="mine"></i>{t('this node')}</span><span><i></i>{t('another validator')}</span><span><i class="empty"></i>{t('nobody (banned, or not known yet)')}</span></p>
      {/if}
      {#if next.length === 0}
        <p class="side-note">{staking?.validator ? t('This node leads none of the slots of this epoch and the next.') : t('This node is not a validator.')}</p>
      {:else}
        <ol class="fb-next" aria-label="Slots this node leads next">
          {#each next as s (s.slot)}
            <li><span>{t('Slot %1', formatCount(s.slot))} <span class="faint fb-small">· {formatTime(s.time)}</span></span><span class="when">{untilText(s.time - clock.now)}</span></li>
          {/each}
        </ol>
      {/if}
    </div>
  </section>
</div>

<div class="side-cols">
  <section class="panel" aria-labelledby="fb-income-title">
    <div class="panel-head">
      <h2 id="fb-income-title">Staking income of this wallet</h2>
      <button type="button" class="btn small ghost" onclick={() => go('transactions')}>Transactions <Icon name="arrow" size={14} /></button>
    </div>
    {#if wallets.current === null || !income}
      <p class="side-empty">Open a wallet to see what it earned from staking.</p>
    {:else}
      <div class="side-table-wrap">
        <table class="side-table" aria-label="Staking income">
          <thead><tr><th></th><th class="end">Today</th><th class="end">In all</th></tr></thead>
          <tbody>
            <tr><td title={t("The producer's share of the fees of the blocks of stake made, paid to this wallet")}>Staked</td><td class="end"><Amount sats={income.today.staked} /></td><td class="end"><Amount sats={income.staked} /></td></tr>
            <tr><td title={t('The fee pot (and a share of the penalties) paid to this wallet for BMM: a BMM commitment it bid for, or a merged mined block')}>BMM rewards</td><td class="end"><Amount sats={income.today.bmm} /></td><td class="end"><Amount sats={income.bmm} /></td></tr>
            <tr><td title={t("The reporter's share of the penalty of a validator that signed two blocks for a slot, for proofs this wallet's blocks carried")}>Slashing rewards</td><td class="end"><Amount sats={income.today.slashing} /></td><td class="end"><Amount sats={income.slashing} /></td></tr>
            <tr><td title={t("The shares by stake of the validator pool (the validators' part of the fees) paid to this wallet as a bond's owner, at the end of each epoch")}>Validator shares</td><td class="end"><Amount sats={income.today.validator} /></td><td class="end"><Amount sats={income.validator} /></td></tr>
            <tr class="total"><td>Total</td><td class="end"><Amount sats={incomeTotal(income, true)} /></td><td class="end"><Amount sats={incomeTotal(income)} /></td></tr>
          </tbody>
        </table>
      </div>
      <p class="side-note fb-small pad">{tn('From %n block(s) of stake whose share this wallet received (%1 today).', income.blocks, formatCount(income.today.blocks))}</p>
    {/if}
  </section>

  <section class="panel" aria-labelledby="fb-coins-title">
    <div class="panel-head"><h2 id="fb-coins-title">Coins of this wallet by finality</h2></div>
    <div class="panel-body">
      {#if wallets.current === null}
        <p class="side-note">Open a wallet to see how final its coins are.</p>
      {:else if !finality}
        <p class="side-note">The node does not say how final the coins are.</p>
      {:else}
        <div class="fb-finality">
          <div class="stack" role="img" aria-label={t('Soft %1 %, BMM-confirmed %2 %, final %3 %', part(finality.soft), part(finality.anchored), part(finality.final))}>
            <span class="final" use:css={{ width: `${part(finality.final)}%` }}></span>
            <span class="anchored" use:css={{ width: `${part(finality.anchored)}%` }}></span>
            <span class="soft" use:css={{ width: `${part(finality.soft)}%` }}></span>
          </div>
          <dl>
            <dt><i class="final"></i><span>Final</span></dt><dd><Amount sats={coinsToSats(finality.final)} /></dd>
            <dt><i class="anchored"></i><span>BMM-confirmed</span></dt><dd><Amount sats={coinsToSats(finality.anchored)} /></dd>
            <dt><i class="soft"></i><span>Soft</span></dt><dd><Amount sats={coinsToSats(finality.soft)} /></dd>
          </dl>
          <p class="side-note fb-small">
            Soft: only blocks of stake are on it, like an unconfirmed payment with slashing behind it. BMM-confirmed: a BMM commitment on the mainchain covers it; undoing it takes the mainchain's merged miners, not stake. Final: enough BMM commitments cover it.
            {#if fb.anchors}{t('The chain is BMM-confirmed up to block %1 and final up to block %2.', formatCount(Math.max(0, fb.anchors.anchoredheight)), fb.anchors.finalheight >= 0 ? formatCount(fb.anchors.finalheight) : '–')}{/if}
          </p>
        </div>
      {/if}
    </div>
  </section>
</div>

<style>
  .total td {
    font-weight: 650;
  }
  .pad {
    padding: 0.6rem 0.8rem 0.9rem;
  }
</style>
