// The block explorer of this chain (pages/Explorer.svelte): columns of its own after the height, facts at the top
// of a block's details, a tag on the boxes of the ribbon, columns of the kit it has no use for. Each reads the
// block as getblock (verbosity 1) gives it, without its transactions; `refresh` is called with each new tip
// while the explorer is shown, for what the columns read besides the blocks.
//
// FastBlocks: getblock says a block's kind ("pos": a block of stake, "bmm": a merged mined block), the slot and
// the producer of a block of stake, the BMM commitment it records (ack), the mainchain block that committed to a
// merged mined one, the proofs of double signing it carries and how final it is (src/rpc/pos.cpp,
// PushPosBlockInfo). Shown as the Qt explorer did: PoS, BMM (a block of stake that records a BMM commitment) or
// PoW (merged mined), and finality as of the BMM-confirmed and final tips of getanchorinfo.
import { t, tn } from '../lib/i18n.svelte';
import { formatCount, shortHash } from '../lib/format';
import { rpc } from '../lib/rpc';
import './fb/fb.css';
import { blockKind, finalityOf, kindText, type FinalityTips } from './fb/model';

export type ChainBlock = Record<string, unknown>;

export interface ChainColumn {
  key: string;
  label: string;
  width: string;
  align?: 'start' | 'end';
  /** The value (what is sorted and shown); undefined while the block is not read. */
  value: (block: ChainBlock) => string | number | undefined;
  /** A badge of the value: its class (badge <tone>), and the words of its tooltip. */
  badge?: (block: ChainBlock) => { tone: string; tip: string } | undefined;
  /** The tooltip of a cell that is not a badge. */
  tip?: (block: ChainBlock) => string | undefined;
  mono?: boolean;
}

export interface ChainFact {
  /** Its term and its description, words of the app (made with t()). */
  dt: string;
  dd: string;
  mono?: boolean;
}

export interface ChainExplorer {
  columns: ChainColumn[];
  /** Columns of the kit's list of blocks this chain leaves out (by key). */
  hide: string[];
  facts: (block: ChainBlock) => ChainFact[];
  /** The tag of a block's box in the ribbon, instead of its coinbase's; undefined: the coinbase's. */
  tag?: (block: ChainBlock) => string | undefined;
  refresh?: () => Promise<void>;
}

/** What the columns read besides the blocks: the BMM-confirmed and final tips, this node's validator keys. */
const known: FinalityTips & { keys: Set<string>; keysAt: number } = { anchored: -1, final: -1, finalAnchors: 1, keys: new Set(), keysAt: 0 };

function producer(block: ChainBlock): string | undefined {
  const key = typeof block.proposer === 'string' ? block.proposer : '';
  if (!key) return undefined;
  return known.keys.has(key) ? t('this node') : shortHash(key, 6);
}

export const EXPLORER: ChainExplorer = {
  columns: [
    {
      key: 'kind',
      label: 'Kind',
      width: '5.2rem',
      value: (b) => kindText(blockKind(b)) || undefined,
      badge: (b) => {
        const kind = blockKind(b);
        if (kind === 'bmm') return { tone: 'fb-kind-bmm', tip: t('A BMM block: a block of stake that records the BMM commitment of mainchain block %1; deposits and withdrawals happen in it', String((b.ack as { mainheight?: number } | undefined)?.mainheight ?? '?')) };
        if (kind === 'pos') return { tone: 'fb-kind-pos', tip: t('A block of stake, signed by the validator that leads its slot') };
        if (kind === 'pow') return { tone: 'fb-kind-pow', tip: t('A merged mined block (proof of work): made only while no blocks of stake come') };
        return undefined;
      },
    },
    { key: 'slot', label: 'Slot', width: '8.5rem', align: 'end', value: (b) => (typeof b.slot === 'number' ? b.slot : undefined) },
    { key: 'producer', label: 'Producer', width: '7.5rem', mono: true, value: (b) => producer(b), tip: (b) => (typeof b.proposer === 'string' ? b.proposer : undefined) },
    {
      key: 'main',
      label: 'Mainchain',
      width: '6.2rem',
      align: 'end',
      value: (b) => {
        const kind = blockKind(b);
        if (kind === 'bmm') return (b.ack as { mainheight?: number } | undefined)?.mainheight;
        if (kind === 'pow' && typeof b.mainheight === 'number') return b.mainheight;
        return undefined;
      },
      tip: (b) => (blockKind(b) === 'bmm' ? t('The mainchain block whose BMM commitment this BMM block records') : blockKind(b) === 'pow' ? t('The mainchain block that committed to this merged mined block') : undefined),
    },
    {
      key: 'finality',
      label: 'Finality',
      width: '8rem',
      value: (b) => {
        const f = finalityOf(b, known);
        return f ? t(f === 'final' ? 'final' : f === 'anchored' ? 'BMM-confirmed' : 'soft') : undefined;
      },
      badge: (b) => {
        const f = finalityOf(b, known);
        if (!f) return undefined;
        return {
          tone: f === 'final' ? 'fb-final' : f === 'anchored' ? 'fb-anchored' : 'fb-soft',
          tip: t('Soft: only blocks of stake are on it. BMM-confirmed: a BMM commitment on the mainchain covers it. Final: enough BMM commitments cover it.'),
        };
      },
    },
  ],
  // A merged mined sidechain's difficulty is the lowest there is: nothing to read in it; the weight is on the
  // boxes of the ribbon (the list keeps the size).
  hide: ['difficulty', 'weight'],
  facts: (b) => {
    const kind = blockKind(b);
    if (!kind) return [];
    const out: ChainFact[] = [];
    if (kind === 'pos' || kind === 'bmm') {
      const who = typeof b.proposer === 'string' ? (known.keys.has(b.proposer) ? t('this node, %1', shortHash(b.proposer, 8)) : b.proposer) : t('unknown');
      out.push({ dt: t('Kind'), dd: t('PoS: a block of stake, slot %1', formatCount(Number(b.slot ?? 0))) });
      out.push({ dt: t('Producer'), dd: who, mono: typeof b.proposer === 'string' && !known.keys.has(b.proposer) });
      const ack = b.ack as { ticket?: string; anchored?: string; mainheight?: number } | undefined;
      out.push({
        dt: t('BMM block'),
        dd: ack
          ? t('yes: it records the BMM commitment of mainchain block %1, which covers block %2. Deposits and withdrawals happen here, and the fee pot goes to the BMM bidder', formatCount(ack.mainheight ?? 0), shortHash(ack.anchored ?? '', 10))
          : t('no (it records no BMM commitment)'),
      });
    } else {
      const main = typeof b.mainheight === 'number' ? b.mainheight : null;
      out.push({ dt: t('Kind'), dd: main !== null ? t('PoW: merged mined, committed at mainchain block %1', formatCount(main)) : t('PoW: merged mined') });
      out.push({ dt: t('BMM block'), dd: t('a merged mined block, made only while no blocks of stake come: its own BMM commitment covers its parent. Deposits and withdrawals happen here, and the fee pot goes to its first output') });
    }
    const f = b.finality as { status?: string; anchors?: number } | undefined;
    if (f && typeof f.status === 'string') {
      const word = f.status === 'final' ? t('final') : f.status === 'anchored' ? t('BMM-confirmed') : t('soft');
      out.push({ dt: t('Finality'), dd: tn('%1 (%n BMM commitment(s) cover it)', f.anchors ?? 0, word) });
    } else {
      out.push({ dt: t('Finality'), dd: t('not on the active chain') });
    }
    const evidence = Array.isArray(b.evidence) ? b.evidence.length : 0;
    if (evidence) out.push({ dt: t('Proofs'), dd: tn('%n proof(s) that a leader signed two blocks for a slot', evidence) });
    return out;
  },
  tag: (b) => {
    const kind = blockKind(b);
    return kind ? kindText(kind) : undefined;
  },
  refresh: async () => {
    try {
      const a = await rpc.raw<{ anchoredheight: number; finalheight: number; finalanchors: number }>('getanchorinfo', [0]);
      known.anchored = a.anchoredheight;
      known.final = a.finalheight;
      known.finalAnchors = a.finalanchors;
    } catch {
      // Not a chain of stake (yet), or the node is busy: the blocks' own finality.
    }
    if (Date.now() - known.keysAt < 60_000) return;
    try {
      const s = await rpc.raw<{ keys: { pubkey: string }[] }>('getstakinginfo', []);
      known.keys = new Set(s.keys.map((k) => k.pubkey));
      known.keysAt = Date.now();
    } catch {
      // Again at the next block.
    }
  },
};
