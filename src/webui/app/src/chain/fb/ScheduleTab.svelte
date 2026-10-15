<script lang="ts">
  // Staking, Schedule: the epochs (each its first slot, when it starts, its set, its seed) and the leader of each
  // slot of an epoch (getleaderschedule): who leads, this node's slots, the slots nobody leads (a banned leader, or
  // a set or a seed not known yet), the ones past and to come.
  import Amount from '../../components/Amount.svelte';
  import Icon from '../../components/Icon.svelte';
  import Pager from '../../components/Pager.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import { stacked } from '../../lib/actions';
  import { errorText } from '../../lib/dialogs.svelte';
  import { formatCount, formatTime } from '../../lib/format';
  import { t } from '../../lib/i18n.svelte';
  import { rpc } from '../../lib/rpc';
  import { clock } from './clock.svelte';
  import Key from './Key.svelte';
  import { stakeSats, untilText, type Schedule, type ValidatorSet } from './model';
  import { fb } from './store.svelte';

  const SIZES = [25, 50, 100];
  /** How many epochs the overview reads, back from the next one (each is a call of the node). */
  const EPOCH_POINTS = [5, 10, 25, 50, 100];

  const pos = $derived(fb.pos);
  const current = $derived(pos?.epoch ?? 0);
  const slotSeconds = $derived(pos?.slotseconds ?? 30);
  const localKeys = $derived(new Set(fb.staking?.keys.map((k) => k.pubkey) ?? []));

  // --- The schedule of an epoch ---
  let chosen = $state<number | null>(null);
  const epoch = $derived(chosen ?? current);
  let read = $state<Schedule | null>(null);
  let readError = $state('');
  $effect(() => {
    const e = epoch;
    // The current epoch and the next are in the store, kept up to date; another is read once.
    const known = fb.schedules.find((s) => s.epoch === e);
    if (known) {
      read = known;
      readError = '';
      return;
    }
    read = null;
    rpc
      .raw<Schedule>('getleaderschedule', [e])
      .then((s) => {
        if (epoch === e) read = s;
      })
      .catch((err) => (readError = errorText(err)));
  });

  let mineOnly = $state(false);
  let comingOnly = $state(false);
  let page = $state(0);
  let size = $state(25);
  const slots = $derived((read?.slots ?? []).filter((s) => (!mineOnly || (s.leader && localKeys.has(s.leader))) && (!comingOnly || s.slot >= (pos?.slot ?? 0))));
  // The page with the current slot, when the current epoch is shown and nothing was paged yet.
  let paged = false;
  $effect(() => {
    if (paged || !pos || epoch !== current || !slots.length) return;
    const at = slots.findIndex((s) => s.slot >= pos.slot);
    if (at > 0) page = Math.floor(at / size);
    paged = true;
  });
  const shown = $derived(slots.slice(page * size, (page + 1) * size));
  const led = $derived((read?.slots ?? []).filter((s) => s.leader && localKeys.has(s.leader)).length);
  const nobody = $derived((read?.slots ?? []).filter((s) => !s.leader).length);

  // --- The epochs ---
  let points = $state(10);
  let epochs = $state<ValidatorSet[]>([]);
  const sets = new Map<number, ValidatorSet>();
  let epochsLoading = $state(false);
  $effect(() => {
    const n = points;
    const top = current + 1;
    void fb.version;
    epochsLoading = true;
    (async () => {
      const out: ValidatorSet[] = [];
      for (let e = top; e >= Math.max(0, top - n + 1); e--) {
        // Past epochs do not change; the current and the next are read again.
        let s = e < current ? sets.get(e) : undefined;
        if (!s) {
          s = await rpc.raw<ValidatorSet>('getvalidators', [e]).catch(() => undefined);
          if (s && e < current) sets.set(e, s);
        }
        if (s) out.push(s);
      }
      if (points === n) epochs = out;
      epochsLoading = false;
    })();
  });
  let epochsPage = $state(0);
  let epochsSize = $state(25);
  const epochsShown = $derived(epochs.slice(epochsPage * epochsSize, (epochsPage + 1) * epochsSize));
  // When a slot starts: slots are evenly spaced from a slot whose time the node gave.
  const base = $derived(fb.schedules[0]?.slots[0]);
  const epochStart = (first: number) => (base ? base.time + (first - base.slot) * slotSeconds : 0);
</script>

<section class="panel" aria-labelledby="fb-schedule-title">
  <div class="panel-head fb-listbar">
    <h2 id="fb-schedule-title">{t('Leader schedule of epoch %1', formatCount(epoch))}</h2>
    <div class="row stepper">
      <button type="button" class="btn small ghost icon" aria-label="Previous epoch" disabled={epoch <= 0} onclick={() => { chosen = epoch - 1 === current ? null : epoch - 1; page = 0; }}><span class="flip"><Icon name="chevron-right" size={15} /></span></button>
      <input type="number" min="0" aria-label="Epoch" value={epoch} onchange={(e) => { const n = Math.max(0, Math.floor(Number((e.currentTarget as HTMLInputElement).value))); chosen = n === current ? null : n; page = 0; }} />
      <button type="button" class="btn small ghost icon" aria-label="Next epoch" onclick={() => { chosen = epoch + 1 === current ? null : epoch + 1; page = 0; }}><Icon name="chevron-right" size={15} /></button>
      {#if chosen !== null}<button type="button" class="btn small" onclick={() => { chosen = null; page = 0; paged = false; }}>Current</button>{/if}
    </div>
  </div>
  {#if readError}
    <p class="side-empty fb-error"><SafeText text={readError} /></p>
  {:else if !read}
    <p class="side-empty">Asking the node…</p>
  {:else}
    <div class="panel-body">
      <div class="fb-facts">
        <div><span>Slots</span><strong class="num">{formatCount(read.slots[0]?.slot ?? 0)}–{formatCount(read.slots[read.slots.length - 1]?.slot ?? 0)}</strong></div>
        <div><span>Starts</span><strong>{read.slots[0] ? formatTime(read.slots[0].time) : '–'}</strong></div>
        <div><span>This node leads</span><strong class="num">{formatCount(led)}</strong></div>
        <div><span>Led by nobody</span><strong class="num">{formatCount(nobody)}</strong></div>
        <div><span>When</span><strong>{epoch < current ? t('past') : epoch === current ? t('now') : t('to come')}</strong></div>
      </div>
    </div>
    <div class="panel-body fb-listbar">
      <label class="check"><input type="checkbox" bind:checked={mineOnly} onchange={() => (page = 0)} /> {t("This node's slots")}</label>
      <label class="check"><input type="checkbox" bind:checked={comingOnly} onchange={() => (page = 0)} /> {t('To come only')}</label>
      <Pager total={slots.length} bind:page bind:size sizes={SIZES} label="slots" />
    </div>
    {#if shown.length === 0}
      <p class="side-empty">{mineOnly ? t('This node leads none of these slots.') : t('No slot to show.')}</p>
    {:else}
      <div class="side-table-wrap">
        <table class="side-table" use:stacked aria-label="Leader schedule">
          <thead><tr><th class="end">Slot</th><th>Starts</th><th>Leader</th><th></th></tr></thead>
          <tbody>
            {#each shown as s (s.slot)}
              <tr class:chosen={s.slot === pos?.slot}>
                <td class="end num">{formatCount(s.slot)}</td>
                <td>{formatTime(s.time)}</td>
                <td>
                  {#if s.leader}<Key key={s.leader} mine={localKeys.has(s.leader)} />
                  {:else if s.banned}<span class="faint">{t('nobody: its leader is banned')}</span> <span class="mono fb-small" title={s.banned} translate="no">{s.banned.slice(0, 10)}…</span>
                  {:else}<span class="faint">{t('nobody (no set or seed known yet, or blocks of stake off)')}</span>{/if}
                </td>
                <td class="end fb-small">
                  {#if pos && s.slot === pos.slot}<span class="status ok">{t('now')}</span>
                  {:else if pos && s.slot > pos.slot}<span class="faint">{t('in %1', untilText(s.time - clock.now))}</span>
                  {:else}<span class="faint">{t('past')}</span>{/if}
                </td>
              </tr>
            {/each}
          </tbody>
        </table>
      </div>
    {/if}
  {/if}
</section>

<section class="panel" aria-labelledby="fb-epochs-title">
  <div class="panel-head fb-listbar">
    <h2 id="fb-epochs-title">Epochs</h2>
    <label><span class="muted">Latest</span>
      <select aria-label="Epochs read" bind:value={points} onchange={() => (epochsPage = 0)}>
        {#each EPOCH_POINTS as n (n)}<option value={n}>{n}</option>{/each}
      </select>
    </label>
    <Pager total={epochs.length} bind:page={epochsPage} bind:size={epochsSize} sizes={SIZES} label="epochs" />
  </div>
  <p class="side-note fb-small pad">{t('An epoch is %1 slots. Its validator set is fixed by a BMM commitment two epochs before; its seed, which orders its leaders, by a mainchain block.', fb.schedules[0]?.slots.length ?? '–')}</p>
  {#if epochs.length === 0}
    <p class="side-empty">{epochsLoading ? t('Asking the node…') : t('No epoch to show.')}</p>
  {:else}
    <div class="side-table-wrap">
      <table class="side-table" use:stacked aria-label="Epochs">
        <thead><tr><th class="end">Epoch</th><th class="end">First slot</th><th>Starts</th><th class="end">Validators</th><th class="end">Total stake</th><th>Seed</th><th>Blocks of stake</th></tr></thead>
        <tbody>
          {#each epochsShown as e (e.epoch)}
            <tr class:chosen={e.epoch === current}>
              <td class="end"><button type="button" class="fb-hash" onclick={() => { chosen = e.epoch === current ? null : e.epoch; page = 0; document.getElementById('fb-schedule-title')?.scrollIntoView({ block: 'nearest' }); }}>{formatCount(e.epoch)}</button>{#if e.epoch === current} <span class="badge accent">{t('now')}</span>{/if}</td>
              <td class="end num">{formatCount(e.firstslot)}</td>
              <td>{formatTime(epochStart(e.firstslot))}</td>
              <td class="end num">{e.known ? formatCount(e.validators.length) : '–'}</td>
              <td class="end">{#if e.known}<Amount sats={stakeSats(e.totalstake)} />{:else}<span class="faint">{t('not fixed yet')}</span>{/if}</td>
              <td>{e.seedknown ? t('known') : t('not yet')}</td>
              <td><span class="status {e.active ? 'ok' : 'wait'}">{e.active ? t('on') : t('off')}</span></td>
            </tr>
          {/each}
        </tbody>
      </table>
    </div>
  {/if}
</section>

<style>
  .stepper input {
    width: 7rem;
    text-align: center;
  }
  .flip {
    display: inline-flex;
    transform: rotate(180deg);
  }
  .pad {
    padding: 0 1.1rem 0.6rem;
  }
  section + section {
    margin-top: 1rem;
  }
</style>
