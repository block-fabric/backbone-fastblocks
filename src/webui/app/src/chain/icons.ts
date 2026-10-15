// Icons of this chain's own pages, beside those of the kit (components/Icon.svelte): line icons on a
// 24 grid with a 1.75 stroke, one SVG path each, named by the pages of chain/local.ts. The template has
// none; a sidechain adds its own here, so that a merge of the template never conflicts with them:
//
//   export const LOCAL_ICONS = { names: 'M4 5h16v14H4z...' } as const satisfies Record<string, string>;
export const LOCAL_ICONS = {} as const satisfies Record<string, string>;
