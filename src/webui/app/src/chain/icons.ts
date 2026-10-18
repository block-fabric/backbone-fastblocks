// Icons of this chain's own pages, beside those of the kit (components/Icon.svelte): line icons on a
// 24 grid with a 1.75 stroke, one SVG path each. The pages of FastBlocks name them in chain/local.ts.
export const LOCAL_ICONS = {
  /** Staking: coins stacked under a key-shaped lock. */
  stake: 'M4 17.5c0 1.4 2.7 2.5 6 2.5s6-1.1 6-2.5M4 17.5v-4c0-1.4 2.7-2.5 6-2.5s6 1.1 6 2.5v4M4 13.5c0 1.4 2.7 2.5 6 2.5s6-1.1 6-2.5M15 8.5V6a3 3 0 0 1 6 0v2.5M14 8.5h8v5h-8z',
  /** Finality & BMM: a stopwatch with a check. */
  finality: 'M12 21a8 8 0 1 0 0-16 8 8 0 0 0 0 16zM10 2.5h4M12 2.5V5M18.4 6.6l1.4-1.4M8.6 13.2l2.4 2.4 4.4-4.6',
  /** Slashing & jail: a gavel (as the app of Layers names it). */
  slash: 'M13.5 4.5l6 6M11.5 6.5l6 6M14.8 5.8l-5.3 5.3M18.2 9.2l-5.3 5.3M11.2 11.8L4 19M3 21h9',
} as const satisfies Record<string, string>;
