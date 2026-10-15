// Helpers of the page scripts of the end-to-end tests of chains-app. The test runner joins this
// file with a script (smoke.js, intro.js) and gives it to the app with -webuitestscript; it runs in
// the page, with the test methods of the bridge (test.log, test.snapshot, test.exit).
const E2E = (() => {
  const failures = [];
  const passed = [];
  const shots = [];
  const host = (method, params = {}) => window.__chains({ kind: 'host', method, params });
  const rpc = (method, params = [], wallet) => window.__chains({ kind: 'rpc', method, params, ...(wallet == null ? {} : { wallet }) });
  const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
  const log = (message) => host('test.log', { message: String(message) });
  const T = () => window.__chainsTest;

  async function waitFor(what, fn, ms = 30000) {
    const until = Date.now() + ms;
    let last;
    while (Date.now() < until) {
      try {
        last = await fn();
        if (last) return last;
      } catch (e) {
        last = e;
      }
      await sleep(100);
    }
    throw new Error(`timed out waiting for ${what} (${last && last.message ? last.message : JSON.stringify(last)})`);
  }

  function check(name, ok, detail = '') {
    if (ok) passed.push(name);
    else failures.push(`${name}${detail ? ': ' + detail : ''}`);
    return log(`${ok ? 'PASS' : 'FAIL'} ${name}${!ok && detail ? ' (' + detail + ')' : ''}`);
  }

  async function snapshot(name) {
    // Let the page settle: fonts, the transition of a theme.
    await sleep(350);
    await new Promise((resolve) => requestAnimationFrame(() => requestAnimationFrame(resolve)));
    await host('test.snapshot', { name });
    shots.push(name);
  }

  function text(selector) {
    const el = document.querySelector(selector);
    return el ? el.textContent : '';
  }

  function buttonByText(label, root = document) {
    return [...root.querySelectorAll('button')].find((b) => b.textContent.trim() === label);
  }

  /** Say how the test went (the runner reads it), without ending the app. */
  async function report() {
    await log(`RESULT ${JSON.stringify({ passed: passed.length, failures, shots })}`);
  }

  async function finish(error) {
    if (error) failures.push(`script error: ${error && error.stack ? error.stack : error}`);
    await report();
    await host('test.exit', { code: failures.length ? 1 : 0 });
  }

  return { host, rpc, sleep, log, T, waitFor, check, snapshot, text, buttonByText, report, finish };
})();
