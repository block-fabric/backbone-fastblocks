<script lang="ts">
  // Staking (the Staking page of fastblocks-qt, Alt+6): this node as a validator (its keys, staking on or off, its
  // bond and share, the slots it leads next, what it made and missed, the income of the wallet, its coins by
  // finality), the wallet's bonds (bond, add to a bond, unbond, unbond all, a bond to another node's key, the proof
  // of possession for one), the validator set of an epoch and every bond on the chain, and the leader schedule of
  // the epochs. What the node says comes from fb/store (getposinfo, getstakinginfo, getvalidators, getleaderschedule,
  // listbonds); bonds and staking off go through the host, which asks the desktop first (local.cpp).
  import Amount from '../../components/Amount.svelte';
  import Icon from '../../components/Icon.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import { formatCount } from '../../lib/format';
  import { t } from '../../lib/i18n.svelte';
  import { nav } from '../../lib/nav.svelte';
  import { node, wallets } from '../../lib/state.svelte';
  import { current, refreshRecords } from '../../lib/walletdata.svelte';
  import { coinsToSats } from '../../lib/units';
  import '../fb/fb.css';
  import { clock, startClock } from '../fb/clock.svelte';
  import { incomeOf, midnightOf, nodeState, nodeStateText, sharePercent, formatShare, slotsLed, stateTone, untilText } from '../fb/model';
  import { fb, followTip, startFb } from '../fb/store.svelte';
  import BondsTab from '../fb/BondsTab.svelte';
  import NodeTab from '../fb/NodeTab.svelte';
  import ScheduleTab from '../fb/ScheduleTab.svelte';
  import ValidatorsTab from '../fb/ValidatorsTab.svelte';

  $effect(() => startFb());
  $effect(() => startClock());
  $effect(() => {
    void node.tip?.hash;
    void wallets.current;
    followTip();
  });
  // The wallet's history: its staking income.
  $effect(() => {
    if (wallets.current !== null) void refreshRecords();
  });

  const TABS = [
    { id: 'node', label: 'This node' },
    { id: 'bonds', label: 'Bonds' },
    { id: 'validators', label: 'Validators' },
    { id: 'schedule', label: 'Schedule' },
  ];
  const tab = $derived(TABS.some((x) => x.id === nav.tab) ? nav.tab : 'node');

  const pos = $derived(fb.pos);
  const staking = $derived(fb.staking);
  const state = $derived(nodeState(staking, pos?.epoch ?? 0));
  const tone = $derived(state === 'nokey' ? 'wait' : state === 'error' || state === 'off' ? 'bad' : stateTone(state));
  const localKeys = $derived(new Set(staking?.keys.map((k) => k.pubkey) ?? []));
  const bonded = $derived((staking?.keys ?? []).reduce((n, k) => n + coinsToSats(k.bond?.bonded ?? 0), 0n));
  const unbonding = $derived((staking?.keys ?? []).reduce((n, k) => n + coinsToSats(k.bond?.unbonding ?? 0), 0n));
  const share = $derived(staking ? sharePercent(staking.stake, staking.totalstake) : 0);
  const next = $derived(pos ? slotsLed(fb.schedules, localKeys, pos.slot + 1, 1)[0] : undefined);
  const income = $derived(wallets.current === null ? null : incomeOf(current.records, midnightOf(new Date(clock.now * 1000))));
  const todayIncome = $derived(income ? income.today.staked + income.today.bmm + income.today.slashing : 0n);
  const allIncome = $derived(income ? income.staked + income.bmm + income.slashing : 0n);
</script>

<div class="page fb-page staking">
  <header class="page-head">
    <h1>Staking</h1>
    {#if pos}
      <span class="muted fb-head-meta">
        <span>{t('Slot %1', formatCount(pos.slot))}</span>
        <span>{t('epoch %1', formatCount(pos.epoch))}</span>
        <span>{t('a slot every %1 s', pos.slotseconds)}</span>
      </span>
    {/if}
  </header>

  {#if fb.error}
    <p class="side-note side-bad" role="alert"><SafeText text={fb.error} /></p>
  {:else if pos && !pos.active}
    <p class="side-note side-warn" role="status">
      Blocks of stake are off in this epoch: too few validators have stake bonded. The chain's blocks are merged mined only until enough is bonded.
    </p>
  {:else if pos?.stalled}
    <p class="side-note side-warn" role="status">No blocks of stake lately: merged mined blocks carry the transactions.</p>
  {/if}

  <section class="side-stats five" aria-label="This node as a validator">
    <div class="panel side-stat">
      <span class="muted">This node</span>
      <strong><span class="status {tone}" id="fb-node-state">{nodeStateText(state)}</span></strong>
      <span class="faint">{staking?.validator ? (staking.enabled ? t('staking on') : t('staking off')) : t('no validator key')}</span>
    </div>
    <div class="panel side-stat">
      <span class="muted">Bonded</span>
      <strong class="num"><Amount sats={bonded} /></strong>
      <span class="faint">{#if unbonding > 0n}<Amount sats={unbonding} /> {t('unbonding')}{:else}{t('its keys, in the state of the chain')}{/if}</span>
    </div>
    <div class="panel side-stat">
      <span class="muted">Share of the set</span>
      <strong class="num">{formatShare(share)}</strong>
      <span class="faint">{staking && staking.totalstake > 0 ? t('about %1 of the slots', formatShare(share)) : t('no set known for this epoch')}</span>
    </div>
    <div class="panel side-stat">
      <span class="muted">Next slot it leads</span>
      <strong class="num">{next ? untilText(next.time - clock.now) : '–'}</strong>
      <span class="faint">{next ? t('slot %1', formatCount(next.slot)) : staking?.validator ? t('none in the next two epochs') : t('not a validator')}</span>
    </div>
    <div class="panel side-stat">
      <span class="muted">Income today</span>
      <strong class="num">{#if income}<Amount sats={todayIncome} />{:else}–{/if}</strong>
      <span class="faint">{#if income}{t('in all:')} <Amount sats={allIncome} />{:else}{t('no wallet open')}{/if}</span>
    </div>
  </section>

  <div class="tabs" role="tablist" aria-label="Staking">
    {#each TABS as tb (tb.id)}
      <button type="button" role="tab" aria-selected={tab === tb.id} onclick={() => (nav.tab = tb.id)}>
        {t(tb.label)}
        {#if tb.id === 'bonds' && fb.bonds?.length}<span class="badge accent">{fb.bonds.length}</span>{/if}
        {#if tb.id === 'validators' && fb.set?.validators.length}<span class="badge">{fb.set.validators.length}</span>{/if}
      </button>
    {/each}
  </div>

  {#if !fb.loaded}
    <p class="muted"><Icon name="clock" size={15} /> Asking the node…</p>
  {:else if tab === 'node'}
    <NodeTab {income} />
  {:else if tab === 'bonds'}
    <BondsTab />
  {:else if tab === 'validators'}
    <ValidatorsTab />
  {:else}
    <ScheduleTab />
  {/if}
</div>
