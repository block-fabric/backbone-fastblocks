// The time, in seconds, ticking once a second while a page that counts down is shown (paused when hidden).
import { every } from '../../lib/poll';

export const clock = $state({ now: Math.floor(Date.now() / 1000) });

let users = 0;
let stop: (() => void) | null = null;

export function startClock(): () => void {
  users++;
  if (users === 1) stop = every(1000, () => (clock.now = Math.floor(Date.now() / 1000)), { now: true });
  return () => {
    users = Math.max(0, users - 1);
    if (users === 0 && stop) {
      stop();
      stop = null;
    }
  };
}
