# chains-app: the HTML interface of Chains

`chains-app` is the GUI of Chains: the node with its wallet and an HTML
interface in a native web view, in one program, without Qt. It replaced
`chains-qt`, which was removed from the tree (the commit "Remove the Qt GUI").
What it had to do to replace it is recorded in [webui-parity.md](webui-parity.md);
the audit of it, and how each finding was closed, in
[webui-audit.md](webui-audit.md). Its window comes in three layouts
(dashboard, workspace tabs, compact sidebar) with a command palette and an
integrated title bar: [webui-layouts.md](webui-layouts.md).

This document says how to build and run it, and how it is made, so that the
sidechains can take it over for their own apps.

## Build

```
cmake -B build              # BUILD_WEBUI is ON by default; -DBUILD_WEBUI=OFF for a node without a window
cmake --build build --target chains-app test_chains-app
```

* **Needs**: GTK 3 and WebKitGTK 4.1 (`libwebkit2gtk-4.1-dev`), Node.js with
  npm. The first build runs `npm ci` in src/webui/app (the exact versions of
  package-lock.json); later builds need no network.
* `-DWEBUI_PREBUILT_DIST=<dir>` takes an interface built elsewhere
  (`npm run build` in src/webui/app) and needs no npm.
* The interface is built by `npm run build`: the catalogs of the languages
  first (`scripts/i18n.mjs`, from the catalogs in src/webui/locale; the
  catalogs made are committed in `public/locales`), then Vite. The result is embedded in
  the binary (cmake/embed.cmake → `assets_data.cpp`).
* Windows (WebView2) has its own window (window_win32.cpp, below); macOS
  (WKWebView) builds the generic window (window_generic.cpp) until its port
  (file choosers, tray, frame, App Nap), see the parity list.

On Linux, `cmake --build build --target deploy-app` makes the release as a
tarball (`release-app/chains-app-<version>-<arch>-linux-gnu.tar.gz`: chains-app,
the programs of the node, the desktop entry and the icon, under `bin/` and
`share/`). `cmake --install build` installs the same (component `chains-app`
for the app, its desktop entry and icon; `test_chains-app` goes to libexec).

### Windows

Cross-compiled from Linux (or WSL) with the depends system and mingw-w64 (posix):

```
make -C depends HOST=x86_64-w64-mingw32 -j4
cmake -B build_win --toolchain depends/x86_64-w64-mingw32/toolchain.cmake -DWITH_CCACHE=OFF
cmake --build build_win -j4 --target chains-app test_chains-app
cmake --build build_win --target deploy-app   # zip + NSIS installer in build_win/release-app
```

The depends package `webview2` brings the headers of the Microsoft.Web.WebView2
NuGet package (BSD-3-Clause, its LICENSE.txt in depends/<host>/share/webview2).
No WebView2Loader is linked or shipped: webview.h finds the WebView2 Runtime
itself. The runtime is part of Windows 10 (since 2021) and 11; the app and the
installer point to its download page when it is missing. The window
(window_win32.cpp) has the integrated title bar, the tray icon and its menu,
notifications (balloons of the tray icon, shown as toasts), file dialogs
(IFileDialog), the clipboard, per monitor DPI awareness, the single instance
over a named pipe only the user can open, and registers `chains:` for the user
(HKCU\Software\Classes; `-webuiregisteruri=0` to skip, never with a test
script). Its data (WebView2 caches) are in `%LOCALAPPDATA%\Chains\WebView2`.

Debug options: `-webuidevtools` (web inspector), `-webuiconfig=<file>`,
`-webuitestscript=<file>` and `-webuitestshots=<dir>` (tests). The last two,
and the test methods of the bridge they come with (`test.confirmAnswer`,
`test.fileAnswer`...), are only in a build for the tests:
`cmake -DWEBUI_TEST_HOOKS=ON` (OFF by default). A release is built with it OFF
(`deploy-app` refuses a build that has them, and the ctest
`webui_test_hooks_built` checks the program); the end-to-end tests need it ON.
Working on the interface alone: `npm install` then `npm run dev` in
src/webui/app opens it in a browser with a pretend node (lib/mock.ts).

## Run

```
build/bin/chains-app [options of chainsd] [chains:URI]
```

* The options of chainsd, and the GUI options of chains-qt: `-choosedatadir`,
  `-lang=<lang>`, `-min`, `-resetguisettings`, `-splash`.
* Settings: those of the node stay in `settings.json` of the data directory,
  as with chains-qt. Those of the app (the data directory chosen in the intro,
  the window, the look, the unit, the language of the menus...) are in
  `~/.config/Chains/chains-app.json`, one set per network.
* One app per data directory: a second start hands its `chains:` URIs to the
  first one (single-instance socket in a private folder of the user).
* Debug options, from the command line only (a chains.conf that sets them is
  ignored): `-webuidevtools` (web inspector), `-webuiconfig=<file>` (another
  settings file of the app), `-webuitestscript=<file>` and
  `-webuitestshots=<dir>` (the tests: a script run in the page, with test
  methods on the bridge; only in a build with `WEBUI_TEST_HOOKS=ON`).

## How it is made

```
page (Svelte, chains-app://app/, UI thread)            the user
  │ window.__chains(request)          ▲ window.__chainsEvents(batch)
  ▼                                   │
Bridge                                EventHub (batches, coalesces, throttles)
  ├─ quick lane (4 workers)           ▲
  ├─ long lane (6 workers)            NodeEvents ◄── signals of the node and wallets
  ├─ UI thread (window, clipboard)
  ├─ rpc  → RPC policy → interfaces::Node::executeRpc
  └─ host → methods of the app (app.cpp, walletapi.cpp, nodeapi.cpp, ...)

App, on a thread of its own: intro, configuration, init, then shutdown
```

### The host (src/webui, C++)

| File | What it does |
|------|--------------|
| main.cpp | The command line, as chains-qt: options, payment URIs, `-help`; starts App; translations of the node (`G_TRANSLATION_FUN`) |
| app.cpp | The life of the app: intro, `InitConfig`, single instance, node init and shutdown on a thread, the phases the page follows, dialogs of the node, the host methods of the app (files chosen by the user, clipboard, preferences, options, the window, the tray, notifications), the language, the integrated title bar |
| bridge.cpp | Requests of the page: the RPC policy, the lanes, cancellation, answers made valid UTF-8, secrets wiped after use, the internal error hook |
| rpcpolicy.cpp | The RPC methods the pages may call, and which of them run on the long lane |
| events.cpp | The events to the page: batched, a key coalesces the ones that change fast (the tip during a sync) |
| nodeevents.cpp | Signals of the node and of each wallet made into events; balances polled as WalletModel does |
| walletapi.cpp | The wallet functions of chains-qt (WalletModel, WalletController, the wallet dialogs) as host methods, with the same interfaces::Wallet calls, rules and messages |
| nodeapi.cpp, console.cpp | The node window: the console (RPCParseCommandLine and RPCExecutor without Qt), peers and bans, tools (hashes, merkle trees, decoders, deposit addresses, stamps) |
| options.cpp | The options of the node as OptionsModel keeps them, and `OptionsJournal`: the changes of the Settings page wait for Apply |
| sidechainnodes.cpp | The Sidechain Nodes window: install, start, stop the nodes of sidechains |
| i18n.cpp | The language: chosen as chains-qt chooses it, the catalog of the page, the translation of the node's messages |
| icontint.cpp | The icon of the network (NetworkStyle) |
| assets.cpp | The embedded interface, its content security policy, the inline document of the other platforms |
| appconfig.cpp, singleinstance.cpp | The settings file of the app; one app per data directory |
| window_gtk.cpp, tray_gtk.cpp | The window on Linux: WebKitGTK through webview/webview, file choosers, clipboard, the integrated title bar, the tray (StatusNotifierItem) and notifications |
| window_win32.cpp, urischeme_win.cpp | The window on Windows: WebView2 through webview/webview (the interface served by the web view itself under `https://app.chains-app.invalid/`, a name that never resolves, with the same policy as a header), the integrated title bar (WM_NCCALCSIZE, snap layouts), the tray, toasts, IFileDialog, the clipboard, the session end; `chains:` registered for the user |
| window_generic.cpp | The window of the other platforms (macOS), through webview/webview (to be completed by its port) |

**The bridge.** The page sends `{rid, kind: rpc|host|cancel, method, params,
wallet}`. RPC calls go to the in-process dispatcher, as the console of
chains-qt does: no HTTP server, no credentials. They are limited to the
methods of `rpcpolicy.cpp` (a frontend test checks that every method the pages
name is there); the console (`console.run`) is the one way to run any command.
Calls never run on the UI thread. Calls that may take long (a command of the
console, a download, a rescan, a file chooser, loading a wallet,
`scantxoutset`) run on a lane of their own and never hold up the quick ones. A
call can be cancelled. Every answer and every event batch is made valid UTF-8
(text of the chain is any bytes, and WebKit drops a script that is not UTF-8).
Strings named `passphrase`, `old`, `new` are wiped once a request is handled.
A host method that throws something it did not expect shows the internal error
dialog of chains-qt.

**Keys.** Calls that need the keys of an encrypted wallet take its passphrase
for that call: the host unlocks the wallet, runs the call and locks it again
(`UnlockScope`, as WalletModel::UnlockContext). A locked wallet answers -13 and
the page asks for the passphrase. RPC methods that need keys (deposits, stamps,
signing a PSBT, proofs of funds) go through `wallet.withKeys` the same way; the
page never leaves a wallet unlocked for a while.

**The history.** `wallet.transactions` gives the whole history once (records as
TransactionRecord makes them, as arrays), the records of some transactions
(`txids`: the ones an event named), or the newest (`latest`). Each record says
the height of its block and its state, so the page brings the status of every
record up to date with each block by itself
(TransactionRecord::updateStatus); only transactions out of a block are asked
for again. The balance chart is made by the host (`wallet.balanceHistory`).

**Settings.** The options of the node are written to settings.json as they
change, and what they replaced is kept by `OptionsJournal` (in chains-app.json):
Apply forgets it, Cancel puts it back, and the next start puts back what was
never applied, before the node reads its settings.

**Languages.** The catalogs of Bitcoin Core's translators (src/webui/locale,
about 100 languages, as chains-qt shipped them; see
[translation_process.md](translation_process.md)) are made into catalogs of the app by `scripts/i18n.mjs`: the messages the
interface shows (normalized as it writes them: no mnemonic `&`, `…`, Chains for
Bitcoin, labels without their colon) with their plural forms, and the messages
of the node. The host chooses the language (-lang, the option, the locale of
the system) and gives the catalog to the page (`app.locale`); the page
translates its own words only (opt-in, see Translations below), and the node's
messages go through `G_TRANSLATION_FUN`.

### The interface (src/webui/app, Svelte 5 + TypeScript + Vite)

```
src/
  main.ts, App.svelte     start: hello, language, the title bar, phases (intro, splash, shell, fatal)
  views/                  Shell (the layout chosen, the palette), layouts/ (Dashboard, Workspace,
                          Compact), home/ (the dashboard's Home), PageView, Intro, Splash, Fatal
  pages/                  one per page of chains-qt (Overview, Send, Receive, Transactions,
                          Sidechains, Mine, Explorer, NodeWindow, Settings, Wallets, ...)
  components/             shared parts: DataTable (virtual, sortable), Dialog, DialogHost,
                          AmountInput, AddressInput, MenuButton, CommandPalette, DetailPanel,
                          Sidebar, StatusBar, SyncOverlay, Toasts, TitleStrip, WindowControls,
                          ResizeHandles, ...
  lib/
    bridge.ts             the transport, requests, events
    rpc.ts                typed RPC and host calls
    state.svelte.ts       the state of the app, the node, the wallets, the preferences
    walletdata.svelte.ts  the current wallet: info, history kept up to date, address book
    wallet.ts             wallet calls, record status, filters, CSV
    i18n.svelte.ts        the language: t(), tn() (no translator of the whole page)
    commands.svelte.ts    every action: the pages and the menus of chains-qt (Menus, palette)
    layout.ts, detail.svelte.ts  the layouts; the detail panel
    windowframe.svelte.ts the integrated title bar
    settingsdraft.svelte.ts  the changes of Settings until Apply
    nav.svelte.ts, keyboard.ts, dialogs.svelte.ts, units.ts, format.ts, ...
  styles/                 themes.css (colour tokens of the themes), base.css (shared rules)
  test/                   vitest: pages, components, the audit fixes, contrast, translations
public/locales/           the catalogs of the languages (made by scripts/i18n.mjs)
```

Rules the code keeps:

* **Programs run only on the user's word, given to the desktop.** Whatever
  makes the app run a program (a sidechain downloaded from Sidechain Nodes, the
  external signer `-signer`) goes through `Window::Confirm`, a dialog of the
  window system that the page can neither draw nor answer, and a program path
  only comes from the file chooser of the desktop. Downloads are https only,
  with no user name before the host (`https://a@b/` is refused), with a
  SHA-256 checked before anything is unpacked; the releases built into the app
  (`sidechainreleases.h`) come first. The dialog says first what the app
  decides, in fixed words: its title whether the release is built into the app
  or was typed in it, its first line the host the download comes from (parsed
  by the host), alone on its line; what the page gave (the name of the
  sidechain, the address) only after. The name of a sidechain added by hand is
  one line of at most 64 printable characters (refused otherwise, and cleaned
  up again wherever it is shown). Sidechain nodes get the RPC
  password of this node in an owner-only file (`-mainchainrpcpasswordfile`),
  never on their command line.
* **Text is text.** No `{@html}`, no `innerHTML`: what comes from the chain,
  the node or the user shows as text (a test checks the sources). The content
  security policy allows nothing but the app's own files: no remote content,
  no inline scripts or styles, no `eval`.
* **Translations (opt-in).** Write the interface in English, with the words of
  chains-qt where it has them (they are then translated by its catalogs). Only
  the app's own words are translated, never what the page happens to show:
  - the words of the templates (text, and the title, placeholder, aria-label
    and alt attributes; the label/title/placeholder/empty/text props of
    components; string literals in `{a ? 'X' : 'Y'}`) become calls of `t()`
    when the components are built (`scripts/i18n-preprocess.mjs`); values put
    in them (`{n} blocks behind`) are spliced in as they are, never looked up;
  - words of the app held in code (tabs, menus, titles) are shown with `t()` /
    `tn()`;
  - a message with values (a toast, a dialog, a status, "3 hours behind") is
    translated where it is made, with its own placeholders:
    `t('%1 behind', niceTimeOffset(s))`, `tn('%n hour(s)', n)` ("(s)" is the
    English plural without a catalog). Toasts, dialogs and statuses show what
    they get as it is (`SafeText`): an error of the host, a message of the
    node or the RPC is never looked up, so never partly translated;
  - text from the chain, peers, the wallet or the user (labels, memos,
    comments, sidechain titles, peer versions, URIs, wallet names) is shown
    with `<SafeText text={...} />` (translate="no"), or in an element with
    `translate="no"`. `test/i18n-lint.test.ts` fails on a field of words shown
    bare, on such text put in `t()`, on text put together given to `t()` (a
    call, a template with values, a sum), and on words of the app put in a
    toast or a dialog without `t()`.
  Only the source of a message, as written in the app, is looked up, as a
  whole: nothing is matched in other text. After a change of
  the texts, `npm run i18n` makes the catalogs again (a test checks they are
  up to date).
* **Themes.** Colours are tokens of themes.css (`--text`, `--text-muted`,
  `--text-faint`, `--accent`, `--accent-fg`, ...): every text token reaches
  4.5:1 on every surface of every theme (test/contrast.test.ts measures it).
  Use `--accent-fg`, not `--accent`, for text.
* **Narrow windows.** Pages wrap rather than clip; grids use
  `minmax(min(Xrem, 100%), 1fr)`; below 760 px the dashboard's sidebar is a
  drawer and the compact rail shows icons; the menus are in one Menus button
  that opens inside the window; tables can `fit` the width.
* **Long lists.** Keep them as raw state (`$state.raw`), replaced as a whole,
  and draw them with DataTable (only the rows in view).

## Taking it over for a sidechain

What is generic, and moves as it is: the bridge (and its lanes, policy hook,
UTF-8 repair), the events, the window and the tray of GTK, the assets and their
policy, the settings file of the app, the single instance, the languages, the
title bar, and in the interface: lib/ (bridge, state, i18n, windowframe, nav,
keyboard, dialogs, units, format, commands, layout), the components, the
themes, the layouts and the settings page.

What each chain sets: `CLIENT_NAME` and the scheme of its payment URIs
(main.cpp), its RPC policy (rpcpolicy.cpp: the methods its pages call), its
pages (src/webui/app/src/pages) and their menus (lib/commands.svelte.ts), its
host methods (walletapi.cpp holds the ones of a Bitcoin-like wallet), and in
`scripts/i18n.mjs` the name that takes the place of "Bitcoin" in the catalogs.

## Tests

| Test | What it covers |
|------|----------------|
| `test_chains-app` | The host: the bridge (also in front of a real node), its lanes, policy, UTF-8 repair, wiped secrets and internal errors; events; app settings; assets and the inline document; single instance; node events; the wallet functions on a regtest chain (also without wallets, the history in parts, keys for one call, dust); the node window, options and their journal; the languages; the icon of the network |
| `npm test` (vitest) | The interface: units, addresses, URIs, sync labels, tables, the bridge client, the state, components, wallet helpers, dialogs, pages (send, transactions, settings, sidechains, node window...), the history kept up to date, the RPC policy, the contrast of every theme, the translations, narrow layouts, the fixes of the audit |
| `npm run check` | Types (svelte-check) |
| `e2e/run_smoke.py` | chains-app on regtest in a virtual display, driven through its page: the bridge, a wallet, events, preferences, escaping, shortcuts, a URI handed over by a second start; pictures of every page in each theme |
| `e2e/run_startup.py` | The start of chains-app: a new data directory, then four starts with no wallet to load (the node is ready before the page has its language: the page must take the "ready" event over the older answer of `app.hello`, and the dashboard open its home), then a start with a wallet; with the renderer of a desktop, where the race shows |
| `e2e/run_wallet.py` | The wallet pages as a user drives them |
| `e2e/run_node.py` | The node and Chains pages, with a pretend desktop (tray, notifications) and a second node |
| `e2e/run_audit.py` | The fixes of the audit: the sync overlay, a block built by hand with a title that is not UTF-8, long calls, a history kept up to date in parts, a proof with a locked wallet, contrast measured on what the page draws, aligned tables, the title bar of the desktop or the integrated one, settings that wait for Apply (and the next start), `-disablewallet`, windows 700 and 480 px wide, a hardware wallet (the mock signer of the functional tests), `-lang=fr` |
| `e2e/run_layouts.py` | The three layouts, the palette, the detail panel, the integrated title bar and the refresh (layouts.js, titlebar.js, refresh.js); pictures of each layout |
| `e2e/run_windows.py` | From WSL, on the Windows of the machine (`--app build_win/bin/chains-app.exe --unit build_win/bin/test_chains-app.exe --shots <dir>`, in a folder of the Windows temporary folder, removed at the end): the unit tests, then smoke.js, layouts.js and titlebar.js |

Each e2e script takes `--app build/bin/chains-app --tmp <dir> --shots <dir>`;
all are CTest tests (`webui_tests`, `webui_frontend_tests`,
`webui_frontend_types`, `webui_e2e_smoke`, `webui_e2e_wallet`,
`webui_e2e_node`, `webui_e2e_audit`).

## Credits

The menus and pages follow chains-qt. The translations are the work of the
translators of Bitcoin Core (Transifex), as chains-qt ships them. The tray
speaks the StatusNotifierItem and dbusmenu protocols, and the notifications
the Desktop Notifications specification (freedesktop.org); no code of other
projects is in them. The classic dark theme takes its colours from
QDarkStyleSheet (MIT), as chains-qt does. Fonts: Inter, Nunito, JetBrains Mono
(SIL Open Font License 1.1). webview/webview: MIT.
