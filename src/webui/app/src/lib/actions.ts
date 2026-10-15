// Svelte actions.

/**
 * Set style properties through the CSS object model. The content security policy of the app
 * allows no inline style attributes; properties set from the code are fine.
 */
export function css(node: HTMLElement, props: Record<string, string | number | null | undefined>) {
  let current: Record<string, string | number | null | undefined> = {};
  function apply(next: Record<string, string | number | null | undefined>) {
    for (const key of Object.keys(current)) {
      if (!(key in next)) node.style.removeProperty(key);
    }
    for (const [key, value] of Object.entries(next)) {
      if (value === null || value === undefined) node.style.removeProperty(key);
      else node.style.setProperty(key, String(value));
    }
    current = next;
  }
  apply(props);
  return { update: apply };
}

/**
 * A row of a table that is chosen by a click: the keyboard reaches it (Tab) and chooses it with Enter
 * or Space, as the mouse does.
 */
export function selectable(node: HTMLElement, choose: () => void) {
  let fn = choose;
  node.tabIndex = 0;
  const onkey = (e: KeyboardEvent) => {
    if (e.target !== node) return;
    if (e.key === 'Enter' || e.key === ' ') {
      e.preventDefault();
      fn();
    }
  };
  node.addEventListener('keydown', onkey);
  return {
    update(next: () => void) {
      fn = next;
    },
    destroy() {
      node.removeEventListener('keydown', onkey);
    },
  };
}
