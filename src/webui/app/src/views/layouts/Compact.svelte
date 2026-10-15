<script lang="ts">
  // Compact sidebar: the window of chains-qt refined. An icon rail (names on demand), no menu bar
  // (its items are in the pages, the rail's "Menus" and the palette), denser pages and tables,
  // the detail panel when a card or page asks for it, the status bar.
  import DetailPanel from '../../components/DetailPanel.svelte';
  import Sidebar from '../../components/Sidebar.svelte';
  import StatusBar from '../../components/StatusBar.svelte';
  import { nav } from '../../lib/nav.svelte';
  import SyncOverlay from '../../components/SyncOverlay.svelte';
  import PageView from '../PageView.svelte';
</script>

<div class="compact">
  <Sidebar rail />
  <div class="main">
    <div class="body">
      <!-- Before the page: the dialogs a page opens come after it, above it. -->
      <SyncOverlay />
      <main class="content" id="content" tabindex="-1" data-page={nav.page}>
        <PageView />
      </main>
      <DetailPanel />
    </div>
    <StatusBar />
  </div>
</div>

<style>
  .compact {
    display: flex;
    height: 100%;
  }
  .main {
    flex: 1;
    min-width: 0;
    display: flex;
    flex-direction: column;
  }
  .body {
    position: relative;
    flex: 1;
    min-height: 0;
    display: flex;
  }
  .content {
    flex: 1;
    min-width: 0;
    overflow: auto;
    outline: none;
  }
  /* Denser: less air around pages, panels, tabs and fields. */
  .content :global(.page) {
    padding: 0.9rem 1.1rem 1.1rem;
  }
  .content :global(.page-head h1) {
    font-size: 1.35rem;
  }
  .content :global(.panel-head) {
    padding: 0.65rem 0.9rem 0.4rem;
  }
  .content :global(.panel-body) {
    padding: 0 0.9rem 0.85rem;
  }
  .content :global(.tabs) {
    margin: 0.5rem 0 0.7rem;
  }
  .content :global(.tabs button) {
    padding: 0.4rem 0.75rem;
  }
  .content :global(.btn) {
    min-height: 2.05rem;
  }
  .content :global(input:not([type='checkbox']):not([type='radio']):not([type='range'])),
  .content :global(select) {
    min-height: 2.05rem;
    padding-block: 0.3rem;
  }
  .content :global([role='grid']) {
    font-size: 0.93rem;
  }
  @media (max-width: 760px) {
    .content :global(.page) {
      padding: 0.7rem 0.75rem 0.9rem;
    }
  }
</style>
