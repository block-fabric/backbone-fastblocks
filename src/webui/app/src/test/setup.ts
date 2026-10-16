// Before every test: the app in Advanced mode, as it was before Simple mode came (2026-10-16), since the
// tests of the pages are written for their full form; the tests of Simple mode switch to it themselves.
import { beforeEach } from 'vitest';
import { prefs } from '../lib/state.svelte';

beforeEach(() => {
  prefs.uiMode = 'advanced';
});
