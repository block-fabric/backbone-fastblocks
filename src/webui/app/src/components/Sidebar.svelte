<script lang="ts">
  // The navigation on the left: every page, in sections. Two forms:
  //  - the dashboard's: names and icons, with Home first; on a narrow window a drawer (open);
  //  - the compact layout's rail: icons with tooltips, the names on demand (prefs.sidebarExpanded).
  // At its top a card of the wallet shown (its name, its lock, the unit); the pages in groups; at its
  // foot the menus of chains-qt, the theme, the text size (and the palette in the rail), then
  // Close wallet and Quit, apart. The dashboard's current page is a filled pill; the rail's a bar.
  import { t } from '../lib/i18n.svelte';
  import Brand from './Brand.svelte';
  import Icon from './Icon.svelte';
  import MenuButton from './MenuButton.svelte';
  import PaletteButton from './PaletteButton.svelte';
  import TopActions from './TopActions.svelte';
  import WalletPicker from './WalletPicker.svelte';
  import { css } from '../lib/actions';
  import { buildCommands, menuSections, pageAvailable, pageDisabled, type Command } from '../lib/commands.svelte';
  import { NAV_SECTIONS, pageIcon } from '../lib/layout';
  import { go, nav, PAGES, type PageId } from '../lib/nav.svelte';
  import { prefs, savePrefs, selectWallet, statusTip, wallets } from '../lib/state.svelte';
  import { pendingNow } from '../lib/settingsdraft.svelte';
  import { clampFontAdjust, THEMES, type ThemeId } from '../lib/themes';
  import { frame } from '../lib/windowframe.svelte';

  let { rail = false, open = $bindable(false) }: { rail?: boolean; open?: boolean } = $props();

  let narrow = $state(false);
  $effect(() => {
    if (typeof window.matchMedia !== 'function') return;
    const media = window.matchMedia('(max-width: 760px)');
    narrow = media.matches;
    const onChange = (e: MediaQueryListEvent) => (narrow = e.matches);
    media.addEventListener('change', onChange);
    return () => media.removeEventListener('change', onChange);
  });

  const collapsed = $derived(rail && (!prefs.sidebarExpanded || narrow));
  const sections = $derived([
    ...(rail ? [] : [{ title: '', pages: ['home'] as PageId[] }]),
    ...NAV_SECTIONS.map((s) => ({ title: s.title, pages: s.pages.filter(pageAvailable) })),
  ]);
  const all = $derived(buildCommands());
  const menus = $derived(menuSections(all));
  const looks = $derived([{ title: 'Theme', items: all.filter((c) => c.id.startsWith('node.theme')) }]);
  const walletChoices = $derived([
    {
      title: 'Wallet shown',
      items: wallets.list.map((w): Command => ({ id: `wallet.select.${w.name}`, group: 'Your wallet', label: w.name === '' ? 'default wallet' : w.name, checked: wallets.current === w.name, run: () => selectWallet(w.name) })),
    },
  ]);

  const shownWallet = $derived(wallets.list.find((w) => w.name === wallets.current) ?? null);
  const shownName = $derived(wallets.current ? wallets.current : t('default wallet'));
  const lock = $derived.by(() => {
    const w = shownWallet;
    if (!w) return null;
    if (w.privateKeysDisabled) return { label: 'Watch-only', title: 'Watch-only: no private keys', icon: 'eye' as const, tone: '' };
    if (!w.encrypted) return { label: 'Not encrypted', title: 'The wallet is not encrypted', icon: 'unlock' as const, tone: 'warning' };
    return w.locked
      ? { label: 'Locked', title: 'Encrypted, and locked', icon: 'lock' as const, tone: 'accent' }
      : { label: 'Unlocked', title: 'Encrypted, and unlocked for now', icon: 'unlock' as const, tone: 'warning' };
  });
  const leave = $derived(all.filter((c) => c.id === 'wallet.close' || c.id === 'node.exit'));

  function visit(id: PageId) {
    go(id);
    open = false;
  }

  // Tooltips of the rail: one, placed beside the entry (the rail scrolls, so not inside it).
  let tip: { text: string; top: number; left: number } | null = $state(null);
  function showTip(e: Event, text: string) {
    if (!collapsed) return;
    const r = (e.currentTarget as HTMLElement).getBoundingClientRect();
    tip = { text, top: r.top + r.height / 2, left: r.right + 8 };
  }
  function hideTip() {
    tip = null;
  }
</script>

{#if open}
  <!-- The drawer of a narrow window: a click beside it closes it. -->
  <button type="button" class="scrim" aria-label="Close the navigation" onclick={() => (open = false)}></button>
{/if}

<svelte:window
  onkeydown={(e) => {
    if (e.key === 'Escape' && open) open = false;
  }}
/>

<nav class="sidebar" class:rail class:collapsed class:open aria-label="Pages">
  {#if rail && !narrow}
    <!-- In the compact layout's rail, the logo grows the sidebar (the page names), and shrinks it again. -->
    <Brand
      name={!collapsed}
      expanded={prefs.sidebarExpanded}
      label={prefs.sidebarExpanded ? 'Icons only' : 'Show the names of the pages'}
      onclick={() => savePrefs({ sidebarExpanded: !prefs.sidebarExpanded })}
    />
  {:else if rail || !frame.integrated}
    <!-- The dashboard's name of the app is in the integrated title bar, when there is one. -->
    <Brand name={!collapsed} onclick={() => visit(rail ? 'overview' : 'home')} />
  {/if}

  {#if !collapsed && wallets.list.length > 0}
    <section class="wallet-card" aria-label="Wallet shown">
      <div class="wc-top">
        <span class="avatar" aria-hidden="true"><Icon name="wallet" size={17} /></span>
        <div class="wc-name">
          <span class="caps">Wallet</span>
          <!-- The wallet shown, and the switch between wallets (a round pill); a long name ends in "…",
               whole in its tooltip. The unit of amounts is chosen in the status bar. -->
          {#if wallets.list.length > 1}<WalletPicker />{:else}<span class="name" translate="no" title={shownName}>{shownName}</span>{/if}
        </div>
      </div>
      {#if lock}<div class="wc-foot"><span class="badge {lock.tone}" title={t(lock.title)}><Icon name={lock.icon} size={12} /> {t(lock.label)}</span></div>{/if}
    </section>
  {:else if collapsed && wallets.list.length > 1}
    <div class="center"><MenuButton label="Wallet shown: {wallets.current || 'default wallet'}" icon="wallet" align="start" sections={walletChoices} /></div>
  {/if}

  <div class="scroll">
    {#each sections as section (section.title)}
      <div class="section" role="group" aria-label={t(section.title || 'Home')}>
        {#if section.title && !collapsed}<div class="caption caps">{t(section.title)}</div>{/if}
        {#if section.title && collapsed}<div class="rule" aria-hidden="true"></div>{/if}
        {#each section.pages as id (id)}
          {@const page = PAGES[id]}
          {@const tipText = page.shortcut ? `${page.title} (${page.shortcut})` : page.title}
          <button
            type="button"
            class="entry"
            class:current={nav.page === id}
            aria-current={nav.page === id ? 'page' : undefined}
            aria-label={collapsed ? page.title : undefined}
            disabled={pageDisabled(id)}
            title={collapsed ? undefined : page.summary}
            onclick={() => visit(id)}
            onpointerenter={(e) => {
              showTip(e, tipText);
              statusTip.text = page.summary;
            }}
            onpointerleave={() => {
              hideTip();
              statusTip.text = '';
            }}
            onfocus={(e) => showTip(e, tipText)}
            onblur={hideTip}
          >
            <Icon name={pageIcon(id)} size={19} />
            {#if !collapsed}<span class="grow">{t(page.title)}</span>{/if}
            {#if !collapsed && !page.ready}<span class="soon" title="Being rebuilt">•</span>{/if}
            {#if id === 'settings' && pendingNow() > 0}<span class="pending" title="Changes of the settings wait for Apply" aria-label="Changes wait for Apply"></span>{/if}
          </button>
        {/each}
      </div>
    {/each}
  </div>

  <div class="foot" class:column={collapsed}>
    {#if rail && !frame.integrated}
      <!-- The compact layout has no top bar of its own without the integrated title bar: its round buttons here. -->
      <div class="row" class:column={collapsed}><TopActions /></div>
    {/if}
    {#if collapsed}
      <PaletteButton compact />
      <MenuButton label="Menus of chains-qt: wallet, banking, chains, tools, window, node" sections={menus} placement="right" icon="menu" />
      <MenuButton label="Theme" icon="sun" sections={looks} placement="right" />
    {:else}
      <div class="row">
        {#if rail}<PaletteButton compact />{/if}
        <MenuButton label="Menus of chains-qt: wallet, banking, chains, tools, window, node" sections={menus} placement="above" text="Menus" icon="menu" align="start" />
        <select class="look" aria-label="Theme" title="Theme" value={prefs.theme} onchange={(e) => savePrefs({ theme: (e.currentTarget as HTMLSelectElement).value as ThemeId })}>
          {#each THEMES as theme (theme.id)}
            <option value={theme.id}>{t(theme.name)}</option>
          {/each}
        </select>
      </div>
    {/if}
    <div class="row" class:column={collapsed}>
      <button type="button" class="btn ghost small" title="Smaller text (Ctrl+-)" aria-label="Smaller text" onclick={() => savePrefs({ fontAdjust: clampFontAdjust(prefs.fontAdjust - 1) })}>A−</button>
      <button type="button" class="btn ghost small" title="Larger text (Ctrl++)" aria-label="Larger text" onclick={() => savePrefs({ fontAdjust: clampFontAdjust(prefs.fontAdjust + 1) })}>A+</button>
      {#if rail && !narrow}
        <button
          type="button"
          class="btn ghost small icon toggle"
          aria-expanded={prefs.sidebarExpanded}
          aria-label={prefs.sidebarExpanded ? 'Icons only' : 'Show the names of the pages'}
          title={prefs.sidebarExpanded ? 'Icons only' : 'Show the names of the pages'}
          onclick={() => savePrefs({ sidebarExpanded: !prefs.sidebarExpanded })}
        >
          <Icon name="sidebar" size={16} />
        </button>
      {/if}
    </div>
    <div class="leave" class:column={collapsed}>
      {#each leave as c (c.id)}
        <button
          type="button"
          class="entry leave-entry"
          disabled={c.disabled}
          aria-label={collapsed ? c.label.replace('…', '') : undefined}
          title={c.shortcut ? `${c.label} (${c.shortcut})` : c.label}
          onclick={() => c.run()}
        >
          <Icon name={c.id === 'node.exit' ? 'power' : 'logout'} size={18} />
          {#if !collapsed}<span class="grow">{c.id === 'node.exit' ? 'Quit Chains' : 'Close wallet'}</span>{/if}
        </button>
      {/each}
    </div>
  </div>
</nav>

{#if tip}
  <div class="tip" role="tooltip" use:css={{ top: `${tip.top}px`, left: `${tip.left}px` }}>{t(tip.text)}</div>
{/if}

<style>
  .sidebar {
    display: flex;
    flex-direction: column;
    gap: 0.35rem;
    width: 15rem;
    flex: none;
    padding: 0.7rem 0.6rem 0.6rem;
    min-height: 0;
  }
  /* The compact layout's rail: a surface of its own; the dashboard's sidebar lies on the page. */
  .sidebar.rail {
    border-right: 1px solid var(--line);
    background: color-mix(in srgb, var(--surface) 88%, transparent);
  }
  .sidebar.rail:not(.collapsed) {
    width: 15rem;
  }
  .sidebar.collapsed {
    width: 3.7rem;
    align-items: center;
    padding-inline: 0.35rem;
  }
  .scroll {
    flex: 1;
    min-height: 0;
    overflow-y: auto;
    width: 100%;
    scrollbar-width: thin;
  }
  .center {
    display: flex;
    justify-content: center;
  }
  .caption {
    padding: 0.9rem 0.75rem 0.3rem;
  }
  .wallet-card {
    display: flex;
    flex-direction: column;
    gap: 0.55rem;
    margin: 0.15rem 0 0.3rem;
    padding: 0.75rem;
    border: 1px solid var(--line);
    border-radius: var(--radius-l);
    background: linear-gradient(180deg, var(--card-top), transparent), var(--surface);
    box-shadow: var(--shadow-card);
  }
  .wc-top {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.5rem;
    min-width: 0;
  }
  .avatar {
    display: inline-grid;
    place-items: center;
    width: 2.3rem;
    height: 2.3rem;
    flex: none;
    border-radius: 50%;
    background: var(--accent);
    color: var(--on-accent);
    box-shadow: 0 0 20px -4px var(--accent);
  }
  /* The name takes the room beside the avatar (a narrow sidebar, large text, 125 or 150 % on Windows). */
  .wc-name {
    display: flex;
    flex-direction: column;
    gap: 0.15rem;
    flex: 1 1 5rem;
    min-width: 5rem;
  }
  .wc-name .caps {
    font-size: 0.66rem;
  }
  .wc-name .name {
    font-weight: 650;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  /* The switch between wallets: a round pill, as the other choices of the app. */
  .wc-name :global(select) {
    width: 100%;
    min-width: 0;
    min-height: 1.9rem;
    padding: 0 1.7rem 0 0.75rem;
    border: 1px solid var(--line);
    border-radius: 999px;
    background-color: var(--surface-2);
    font-weight: 650;
    text-overflow: ellipsis;
    white-space: nowrap;
    overflow: hidden;
  }
  .wc-foot {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 0.4rem;
  }
  .wc-foot .badge {
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .leave {
    display: flex;
    flex-direction: column;
    gap: 0.05rem;
    padding-top: 0.35rem;
    border-top: 1px solid var(--line);
  }
  .leave.column {
    align-items: center;
  }
  .leave-entry {
    font-size: 0.92rem;
  }
  .rule {
    height: 1px;
    margin: 0.45rem 0.5rem;
    background: var(--line);
  }
  .section {
    display: flex;
    flex-direction: column;
    gap: 0.08rem;
  }
  .entry {
    position: relative;
    display: flex;
    align-items: center;
    gap: 0.7rem;
    width: 100%;
    padding: 0.5rem 0.75rem;
    border: 0;
    border-radius: var(--radius-m);
    background: none;
    color: var(--text-muted);
    font: inherit;
    font-weight: 550;
    text-align: left;
    cursor: pointer;
  }
  .collapsed .entry {
    justify-content: center;
    padding: 0.55rem;
  }
  .entry:hover:not(:disabled) {
    background: var(--raised);
    color: var(--text);
  }
  /* The dashboard: the current page a filled pill of the accent. */
  .entry.current {
    background: var(--accent);
    color: var(--on-accent);
    box-shadow: 0 6px 20px -8px var(--accent);
  }
  .entry.current:hover {
    background: var(--accent);
    color: var(--on-accent);
  }
  /* The rail: groups in cards, the current page with a bar of the accent at its left. */
  .rail:not(.collapsed) .section {
    margin: 0.2rem 0 0.45rem;
    padding: 0.15rem 0.3rem 0.3rem;
    border: 1px solid var(--line);
    border-radius: var(--radius-l);
    background: var(--surface-2);
  }
  .rail:not(.collapsed) .caption {
    padding: 0.55rem 0.55rem 0.25rem;
  }
  .rail .entry.current,
  .rail .entry.current:hover {
    background: var(--accent-soft);
    color: var(--text);
    box-shadow: none;
  }
  .rail .entry.current::before {
    content: '';
    position: absolute;
    left: -0.35rem;
    top: 0.45rem;
    bottom: 0.45rem;
    width: 3px;
    border-radius: 3px;
    background: var(--accent);
  }
  .rail.collapsed .entry.current::before {
    left: -0.3rem;
  }
  .rail .entry.current :global(svg) {
    color: var(--accent-fg);
  }
  .entry:disabled {
    opacity: 0.45;
    cursor: default;
  }
  .soon {
    color: var(--text-faint);
  }
  .pending {
    flex: none;
    width: 0.5rem;
    height: 0.5rem;
    border-radius: 50%;
    background: var(--warning);
  }
  .collapsed .pending {
    position: absolute;
    top: 0.3rem;
    right: 0.3rem;
  }
  .foot {
    display: flex;
    flex-direction: column;
    gap: 0.35rem;
    padding-top: 0.5rem;
    border-top: 1px solid var(--line);
    width: 100%;
  }
  .foot.column {
    align-items: center;
  }
  .look {
    flex: 1;
    min-width: 6.5rem;
    width: auto;
    min-height: 1.9rem;
    padding-block: 0.15rem;
    font-size: 0.88rem;
  }
  .row {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.2rem;
  }
  .row.column {
    flex-direction: column;
  }
  .toggle {
    margin-left: auto;
  }
  .column :global(.top-actions) {
    flex-direction: column;
  }
  .column .toggle {
    margin-left: 0;
  }
  .tip {
    position: fixed;
    z-index: 50;
    transform: translateY(-50%);
    padding: 0.3rem 0.6rem;
    border-radius: var(--radius-s);
    background: var(--text);
    color: var(--bg);
    font-size: 0.86rem;
    font-weight: 550;
    white-space: nowrap;
    pointer-events: none;
    box-shadow: var(--shadow);
  }
  .scrim {
    display: none;
  }
  @media (max-width: 760px) {
    .sidebar:not(.rail) {
      position: fixed;
      z-index: 36;
      top: 0;
      bottom: 0;
      left: 0;
      width: min(17rem, 85vw);
      transform: translateX(-105%);
      transition: transform 160ms ease-out;
      box-shadow: var(--shadow);
    }
    .sidebar.open {
      transform: none;
    }
    .scrim {
      display: block;
      position: fixed;
      inset: 0;
      z-index: 35;
      border: 0;
      background: var(--overlay);
    }
  }
</style>
