// The app of a sidechain against a live mainchain (run_sidechain.py), driven through its page as a
// user would: a wallet, a deposit address, a deposit made by the helper of the mainchain wallet,
// merged mining turned on, the deposit credited by a block the mainchain committed to, a withdrawal
// to an address of the mainchain wallet, its bundle voted on and paid. Pictures of each step, of every
// page of the sidechain, in the layouts and looks.
(async () => {
  const { rpc, sleep, log, T, waitFor, check, snapshot, text, finish } = E2E;
  const SLOT = window.__E2E_SLOT;
  const MAIN = window.__E2E_MAINCHAIN;
  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];
  const set = (el, value) => {
    el.value = value;
    el.dispatchEvent(new Event('input', { bubbles: true }));
    el.dispatchEvent(new Event('change', { bubbles: true }));
  };
  const openDialog = () => $$('dialog[open]').pop();
  const button = (label, root = document) => $$('button', root).find((b) => b.textContent.trim().replace(/\s+/g, ' ').startsWith(label));
  async function click(label, root) {
    const b = await waitFor(`the button "${label}"`, () => {
      const el = button(label, typeof root === 'function' ? root() : root);
      return el && !el.disabled ? el : null;
    });
    b.click();
    await sleep(80);
  }
  async function answerDialog(title, label) {
    const d = await waitFor(`the dialog "${title}"`, () => {
      const el = openDialog();
      return el && $('h2', el) && $('h2', el).textContent.trim() === title ? el : null;
    });
    await click(label, d);
    await waitFor(`the dialog "${title}" to go`, () => openDialog() !== d || !d.open);
  }
  async function go(page, tab = '') {
    T().go(page, tab);
    await sleep(200);
  }
  const sideRpc = (method, params = []) => rpc(method, params, 'side');
  /** A picture of the page, without the sync overlay a new sidechain shows until its first blocks. */
  async function shot(name) {
    const overlay = $$('dialog[open]').find((d) => /Synchronizing/.test(d.textContent));
    if (overlay) {
      const hide = button('Hide', overlay);
      if (hide) hide.click();
      await sleep(300);
    }
    await snapshot(name);
  }

  try {
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip, 120000);
    const info = await waitFor('the node to reach the mainchain', async () => {
      const i = await rpc('getmainchaininfo');
      return i.connected ? i : null;
    }, 60000);
    await check('the node follows the mainchain, in its slot', info.slot === SLOT && info.connected === true, JSON.stringify(info));
    await check('the dashboard and Midnight by default', T().prefs.layout === 'dashboard' && (T().prefs.theme ?? 'midnight') === 'midnight', `${T().prefs.layout} ${T().prefs.theme}`);

    // A wallet.
    await rpc('createwallet', ['side']);
    await waitFor('the wallet in the page', () => T().wallets.current === 'side');
    await go('home');
    await sleep(1500);
    await shot('sc-home-empty');

    // --- Deposit: a deposit address, then the helper of the mainchain wallet ---
    await go('peg', 'deposit');
    await waitFor('the connection to the mainchain', () => text('.side-bridge').includes('Connected'), 60000);
    await click('New deposit address');
    const deposit = await waitFor('the deposit address', () => {
      const t = text('#deposit-address').trim();
      return t.startsWith(`s${SLOT}_`) ? t : null;
    });
    await check('a deposit address of the slot', deposit.startsWith(`s${SLOT}_`), deposit);
    await sleep(1500);
    await shot('sc-peg-deposit-address');
    set($('#deposit-amount'), '5');
    await sleep(200);
    $('#deposit-helper').click();
    await answerDialog(`Deposit from ${MAIN}`, 'Deposit');
    await waitFor('the deposit on its way', () => text('.peg').includes('On its way'), 60000);
    await check('the deposit is on its way', true);
    await shot('sc-peg-deposit-on-its-way');

    // --- Merged mining: a block with every block of the mainchain ---
    await go('bmm');
    await waitFor('the merged mining page', () => $('#bmm-start'));
    $('#bmm-always').click();
    await sleep(150);
    await click('Start mining');
    await waitFor('mining on', () => text('.bmm').includes('Mining'), 30000);
    const bmm = await rpc('getbmminfo');
    await check('merged mining on, always', bmm.mining === true && bmm.always === true, JSON.stringify(bmm));
    await waitFor('a block of this chain', () => T().node.tip.height >= 1, 120000);
    await check('the mainchain commits to blocks of this chain', true);

    // --- The deposit credited ---
    await waitFor('the deposit credited', async () => (await sideRpc('getbalances')).mine.trusted >= 5, 180000);
    await check('the deposit credited to the wallet', true);
    await go('bmm');
    await waitFor('the blocks committed', () => $$('.side-blocks span.hit').length > 0, 60000);
    await sleep(800);
    await shot('sc-bmm-mining');
    await go('peg', 'deposit');
    await waitFor('the deposit credited in the list', () => text('.peg').includes('Credited'), 60000);
    await shot('sc-peg-deposit-credited');
    await go('home');
    await sleep(2000);
    await shot('sc-home-funded');

    // --- Withdraw to an address of the mainchain wallet ---
    await go('peg', 'withdraw');
    await click(`An address of the ${MAIN} wallet`);
    const payTo = await waitFor('an address of the mainchain wallet', () => $('#withdraw-address') && $('#withdraw-address').value);
    set($('#withdraw-amount'), '1');
    await waitFor('the address checked', () => !text('.peg').includes('This is an address of'));
    await sleep(500);
    $('#withdraw-button').click();
    await answerDialog('Confirm the withdrawal', 'Withdraw');
    await waitFor('the withdrawal in a block', async () => (await rpc('listwithdrawals')).length === 1, 120000);
    await check('the withdrawal waits for a bundle', true);
    await waitFor('the withdrawal in the list', () => $$('table[aria-label="Withdrawals not paid yet"] tbody tr').length === 1, 30000);
    await shot('sc-peg-withdraw');
    await log(`WITHDRAWN ${payTo} 1`);

    // --- The bundle: voted on by the miners of the mainchain, paid ---
    await go('peg', 'bundle');
    await waitFor('the bundle being voted on', async () => (await rpc('getwithdrawalbundle')).status === 'pending', 180000);
    await waitFor('its votes on the mainchain', () => / of 30/.test(text('.peg .bundle')), 120000);
    await check('the votes of the bundle show', true);
    await shot('sc-peg-bundle-voting');
    await waitFor('the bundle paid', () => $$('table[aria-label="Bundles"] .status.ok').length > 0, 300000);
    await check('the bundle paid out', true);
    await waitFor('no withdrawal left', async () => (await rpc('listwithdrawals')).length === 0, 120000);
    await sleep(800);
    await shot('sc-peg-bundle-paid');

    // --- Every page of the sidechain, in the layouts and the looks ---
    await go('home');
    await sleep(1500);
    await shot('sc-home-midnight');
    for (const [page, tab, name] of [['settings', 'mainchain', 'sc-settings-mainchain'], ['node', 'info', 'sc-node'], ['explorer', '', 'sc-explorer'], ['overview', '', 'sc-overview'], ['tools', '', 'sc-tools']]) {
      await go(page, tab);
      await sleep(800);
      await shot(name);
    }
    T().savePrefs({ theme: 'light' });
    for (const [page, tab] of [['home', ''], ['peg', 'deposit'], ['peg', 'bundle'], ['bmm', '']]) {
      await go(page, tab);
      await sleep(900);
      await shot(`sc-${page}${tab ? '-' + tab : ''}-light`);
    }
    T().savePrefs({ theme: 'midnight', layout: 'workspace' });
    await go('peg', 'withdraw');
    await sleep(900);
    await shot('sc-workspace-peg');
    T().savePrefs({ layout: 'sidebar' });
    await go('bmm');
    await sleep(900);
    await shot('sc-compact-bmm');
    T().savePrefs({ layout: 'dashboard', theme: 'ocean' });
    await go('home');
    await sleep(1200);
    await shot('sc-home-ocean');
    T().savePrefs({ theme: 'midnight' });

    // Merged mining off again.
    await go('bmm');
    await click('Stop mining');
    await waitFor('mining off', async () => (await rpc('getbmminfo')).mining === false, 30000);
    await check('merged mining off', true);
    await finish();
  } catch (e) {
    await finish(e);
  }
})();
