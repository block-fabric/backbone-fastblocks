<script lang="ts">
  // Staking, Validators: the validator set of an epoch as the active chain fixes it (getvalidators: stake, share,
  // slots it leads, jailed or banned since), the current epoch's by default, any other by its number; and every
  // validator in the state of the chain (listbonds with all: bonded, unbonding, owner, jailed, banned), a page at a
  // time.
  import Amount from '../../components/Amount.svelte';
  import Icon from '../../components/Icon.svelte';
  import Pager from '../../components/Pager.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import { stacked } from '../../lib/actions';
  import { errorText } from '../../lib/dialogs.svelte';
  import { formatCount } from '../../lib/format';
  import { t } from '../../lib/i18n.svelte';
  import { rpc } from '../../lib/rpc';
  import { wallets } from '../../lib/state.svelte';
  import { coinsToSats } from '../../lib/units';
  import Key from './Key.svelte';
  import Share from './Share.svelte';
  import { sharePercent, stakeSats, stateText, stateTip, stateTone, validatorState, type ValidatorSet, type WalletBond } from './model';
  import { fb } from './store.svelte';

  const SIZES = [25, 50, 100];

  const pos = $derived(fb.pos);
  const current = $derived(pos?.epoch ?? 0);
  /** The epoch shown: null for the current one (it follows the chain). */
  let chosen = $state<number | null>(null);
  const epoch = $derived(chosen ?? current);
  let other = $state<ValidatorSet | null>(null);
  let otherError = $state('');

  $effect(() => {
    const e = chosen;
    other = null;
    otherError = '';
    if (e === null) return;
    rpc
      .raw<ValidatorSet>('getvalidators', [e])
      .then((s) => {
        if (chosen === e) other = s;
      })
      .catch((err) => {
        if (chosen === e) otherError = errorText(err);
      });
  });
  const set = $derived(chosen === null ? fb.set : other);

  let filter = $state('');
  let page = $state(0);
  let size = $state(25);
  const rows = $derived((set?.validators ?? []).filter((v) => !filter.trim() || v.pubkey.includes(filter.trim().toLowerCase())));
  const shown = $derived(rows.slice(page * size, (page + 1) * size));

  // Every validator in the state of the chain (a wallet is needed to ask: listbonds is a wallet method).
  let all = $state<WalletBond[] | null>(null);
  let allError = $state('');
  let allPage = $state(0);
  let allSize = $state(25);
  $effect(() => {
    const wallet = wallets.current;
    void fb.version;
    if (wallet === null) {
      all = null;
      return;
    }
    rpc
      .raw<WalletBond[]>('listbonds', [true], { wallet })
      .then((b) => {
        all = b;
        allError = '';
      })
      .catch((e) => (allError = errorText(e)));
  });
  const allRows = $derived((all ?? []).slice().sort((a, b) => b.bonded + (b.genesisstake ?? 0) - (a.bonded + (a.genesisstake ?? 0))));
  const allShown = $derived(allRows.slice(allPage * allSize, (allPage + 1) * allSize));
  const localKeys = $derived(new Set(fb.staking?.keys.map((k) => k.pubkey) ?? []));
</script>

<section class="panel" aria-labelledby="fb-set-title">
  <div class="panel-head fb-listbar">
    <h2 id="fb-set-title">{t('Validator set of epoch %1', formatCount(epoch))}</h2>
    <div class="row stepper">
      <button type="button" class="btn small ghost icon" aria-label="Previous epoch" disabled={epoch <= 0} onclick={() => (chosen = epoch - 1)}><span class="flip"><Icon name="chevron-right" size={15} /></span></button>
      <input type="number" min="0" aria-label="Epoch" value={epoch} onchange={(e) => { const n = Math.max(0, Math.floor(Number((e.currentTarget as HTMLInputElement).value))); chosen = n === current ? null : n; page = 0; }} />
      <button type="button" class="btn small ghost icon" aria-label="Next epoch" onclick={() => (chosen = epoch + 1 === current ? null : epoch + 1)}><Icon name="chevron-right" size={15} /></button>
      {#if chosen !== null}<button type="button" class="btn small" onclick={() => (chosen = null)}>Current</button>{/if}
    </div>
  </div>
  <div class="panel-body">
    {#if otherError}
      <p class="fb-error"><SafeText text={otherError} /></p>
    {:else if !set}
      <p class="muted">Asking the node…</p>
    {:else}
      <div class="fb-facts">
        <div><span>First slot</span><strong class="num">{formatCount(set.firstslot)}</strong></div>
        <div><span>Set fixed by the chain</span><strong>{set.known ? t('yes') : t('not yet')}</strong></div>
        <div><span>Seed known</span><strong>{set.seedknown ? t('yes') : t('not yet')}</strong></div>
        <div><span>Blocks of stake</span><strong><span class="status {set.active ? 'ok' : 'wait'}">{set.active ? t('on') : t('off: too few validators')}</span></strong></div>
        <div><span>Total stake</span><strong><Amount sats={stakeSats(set.totalstake)} /></strong></div>
        <div><span>Validators</span><strong class="num">{formatCount(set.validators.length)}</strong></div>
      </div>
    {/if}
  </div>
  {#if set && set.validators.length}
    <div class="panel-body fb-listbar">
      <input class="find mono" type="search" placeholder="Find a key" aria-label="Find a key" bind:value={filter} oninput={() => (page = 0)} />
      <Pager total={rows.length} bind:page bind:size sizes={SIZES} label="validators" />
    </div>
    <div class="side-table-wrap">
      <table class="side-table" use:stacked aria-label="Validator set">
        <thead><tr><th>Key</th><th class="end">Stake</th><th>Share</th><th class="end">Slots</th><th>Status</th></tr></thead>
        <tbody>
          {#each shown as v (v.pubkey)}
            {@const st = v.banned ? 'banned' : v.jaileduntilepoch > epoch ? 'jailed' : 'active'}
            <tr>
              <td><Key key={v.pubkey} mine={v.local} /></td>
              <td class="end"><Amount sats={stakeSats(v.stake)} /></td>
              <td><Share percent={sharePercent(v.stake, set.totalstake)} /></td>
              <td class="end num">{formatCount(v.slots)}</td>
              <td><span class="status {stateTone(st)}" title={stateTip(st)}>{st === 'jailed' ? t('Jailed until epoch %1', v.jaileduntilepoch) : st === 'banned' ? t('Banned') : t('In the set')}</span></td>
            </tr>
          {/each}
        </tbody>
      </table>
    </div>
  {:else if set}
    <p class="side-empty">{set.known ? t('The set of this epoch is empty: blocks are merged mined only.') : t('The chain does not fix the set of this epoch yet: it is made at a BMM commitment before the epoch before it.')}</p>
  {/if}
</section>

<section class="panel" aria-labelledby="fb-all-title">
  <div class="panel-head fb-listbar">
    <h2 id="fb-all-title">Every validator on the chain</h2>
    {#if all}<Pager total={allRows.length} bind:page={allPage} bind:size={allSize} sizes={SIZES} label="validators" />{/if}
  </div>
  {#if wallets.current === null}
    <p class="side-empty">Open a wallet to list every bond in the state of the chain.</p>
  {:else if allError}
    <p class="side-empty fb-error"><SafeText text={allError} /></p>
  {:else if !all}
    <p class="side-empty">Asking the node…</p>
  {:else if all.length === 0}
    <p class="side-empty">No validator has bonded yet.</p>
  {:else}
    <div class="side-table-wrap">
      <table class="side-table" use:stacked aria-label="Every validator on the chain">
        <thead><tr><th>Key</th><th class="end">Bonded</th><th class="end">Unbonding</th><th>State</th><th>Owner</th></tr></thead>
        <tbody>
          {#each allShown as b (b.validator)}
            {@const st = validatorState(b, b.inset, current)}
            <tr>
              <td><Key key={b.validator} mine={localKeys.has(b.validator)} /></td>
              <td class="end"><Amount sats={coinsToSats(b.bonded)} />{#if b.genesisstake}<span class="faint fb-small"><br />{t('genesis stake %1', String(b.genesisstake))}</span>{/if}</td>
              <td class="end"><Amount sats={coinsToSats(b.unbonding)} /></td>
              <td><span class="status {stateTone(st)}" title={stateTip(st)}>{stateText(st, b)}</span></td>
              <td class="cut mono fb-small" title={b.owner}>{#if b.owner}<span translate="no">{b.owner}</span>{:else}<span class="faint">{t('genesis validator')}</span>{/if}</td>
            </tr>
          {/each}
        </tbody>
      </table>
    </div>
  {/if}
</section>

<style>
  .stepper input {
    width: 13ch;
    font-variant-numeric: tabular-nums;
    appearance: textfield;
    text-align: center;
  }
  .stepper input::-webkit-inner-spin-button,
  .stepper input::-webkit-outer-spin-button {
    -webkit-appearance: none;
    margin: 0;
  }
  .flip {
    display: inline-flex;
    transform: rotate(180deg);
  }
  .find {
    max-width: 22rem;
    flex: 1;
  }
  section + section {
    margin-top: 1rem;
  }
</style>
