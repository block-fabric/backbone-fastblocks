// FastBlocks in Simple mode (run_sidechain.py --script fastblocks_simple.js --js-var SIMPLE=true, with the
// options of fastblocks.js: this node a validator of the genesis set). Simple mode as a user chooses it (a first start is in Advanced); the home's one card,
// Stake and earn; the Staking card says what the node does in words; a stake from the wallet through the card
// (Start staking, Add more, the estimate of a day), then Stop staking, the desktop asked as a danger. Pictures of
// the home and the card in Midnight and Light.
(async () => {
  const { host, rpc, sleep, T, waitFor, check, snapshot, text, finish } = E2E;
  const X0 = '100f6d8cbf94afb6fc58e9c384b9b3a6516091373a83c869f4e24a9d2bb4a494';
  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];
  const set = (el, value) => {
    el.value = value;
    el.dispatchEvent(new Event('input', { bubbles: true }));
    el.dispatchEvent(new Event('change', { bubbles: true }));
  };
  async function clickId(id, ms = 30000) {
    const b = await waitFor(`#${id} enabled`, () => {
      const el = document.getElementById(id);
      return el && !el.disabled ? el : null;
    }, ms);
    b.click();
    await sleep(150);
  }
  async function shot(name) {
    const overlay = $$('dialog[open]').find((d) => /Synchronizing/.test(d.textContent));
    if (overlay) {
      const hide = $$('button', overlay).find((b) => b.textContent.trim() === 'Hide');
      if (hide) hide.click();
    }
    await sleep(600);
    await snapshot(`fbs-${name}`);
  }
  const W_ = 'fb';
  try {
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip, 120000);
    await check('Advanced on a first start (user decision), Simple once chosen', window.__E2E_FIRST_MODE === 'advanced' && T().prefs.uiMode === 'simple', `${window.__E2E_FIRST_MODE} ${T().prefs.uiMode}`);
    await waitFor('the node to reach the mainchain', async () => (await rpc('getmainchaininfo')).connected, 60000);
    await rpc('createwallet', [W_]);
    await waitFor('the wallet in the page', () => T().wallets.current === W_);
    const deposit = await rpc('getdepositaddress', [], W_);
    await host('test.confirmAnswer', { yes: true });
    await host('peg.deposit', { destination: deposit.depositaddress, amount: '20' });
    await waitFor('blocks of stake', async () => {
      const p = await rpc('getposinfo');
      return p.active && p.lastposslot >= 0 && p.anchors > 0;
    }, 180000);
    await waitFor('the deposit credited', async () => (await rpc('getbalances', [], W_)).mine.trusted >= 20, 240000);
    await check('the pages of Advanced out of the sidebar', !/Finality|Slashing/.test(text('nav.sidebar .scroll')) && /Staking/.test(text('nav.sidebar .scroll')), text('nav.sidebar .scroll'));

    // --- The home: one card, in words ---
    T().savePrefs({ tipsDone: false });
    T().go('home');
    // (The genesis validator may be jailed a few epochs of 20 s while the chain waited for its first BMM commitments.)
    await waitFor('the card says Earning', () => text('#fb-home-simple-state') === 'Earning', 150000);
    await check('the home has one card of FastBlocks, in words', $$('.fb-cards .card').length === 1 && !text('.fb-cards').includes('Next slots'));
    await shot('home-midnight');

    // --- Staking: the card ---
    T().go('staking');
    await waitFor('the card', () => $('#fb-simple-state'), 30000);
    await check('Staking says Earning', text('#fb-simple-state') === 'Earning', text('#fb-simple-state'));
    await check('the validators and the schedule are not shown', !$$('[role="tab"]').some((t) => /Validators|Schedule/.test(t.textContent)));
    set(await waitFor('the amount', () => $('#fb-simple-amount')), '2');
    // An estimate of a day, said to be one; on regtest the blocks pay next to no fees, and it says so.
    await waitFor('the estimate', () => text('#fb-simple-estimate') !== '', 30000);
    await check('an estimate of a day, said to be one, or that fees are too low to earn', /You'd earn about .* a day\. An estimate|next to no fees/.test(text('#fb-simple-estimate')), text('#fb-simple-estimate'));
    await check('a first stake is Start staking', text('#fb-simple-stake').includes('Start staking'));
    await shot('stake-midnight');
    await host('test.confirmAnswer', { yes: true });
    await clickId('fb-simple-stake');
    await waitFor('the stake in the wallet', async () => (await rpc('listbonds', [], W_)).some((b) => b.validator === X0 && b.bonded >= 2), 120000);
    await check('staked to this node\'s key through the card', true);
    await waitFor('Add more', () => text('#fb-simple-stake').includes('Add more'), 60000);
    set($('#fb-simple-amount'), '1');
    await waitFor('the estimate again', () => text('#fb-simple-estimate') !== '', 30000);
    T().savePrefs({ theme: 'light' });
    await shot('stake-light');
    T().go('home');
    await waitFor('the card', () => text('#fb-home-simple-state') !== '', 30000);
    await shot('home-light');
    T().savePrefs({ theme: 'midnight' });

    // --- Stop staking: the desktop asked, as a danger ---
    T().go('staking');
    const before = (await host('test.confirmed')).length;
    await host('test.confirmAnswer', { yes: true });
    await clickId('fb-simple-stop', 60000);
    const asked = await waitFor('the desktop asked', async () => {
      const c = await host('test.confirmed');
      return c.length > before ? c[c.length - 1] : null;
    }, 30000);
    await check('Stop staking is an unbond, confirmed as a danger', asked.title === 'Unbond' && asked.level === 'danger', JSON.stringify({ title: asked.title, level: asked.level }));
    await waitFor('the stake coming back', async () => (await rpc('listbonds', [], W_)).some((b) => b.validator === X0 && b.unbonding > 0), 120000);
    await check('the stake on its way back', true);
    await sleep(2000);
    await shot('stopping-midnight');
    await finish();
  } catch (e) {
    await finish(e);
  }
})();
