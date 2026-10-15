// The tools of chains-qt that work on text: proofs of funds, multisig amounts and signature counts,
// the search of the block explorer, the payload of a file stamp.

import { t } from './i18n.svelte';

// --- Proof of funds (proofoffundsdialog.cpp) ---

export const PROOF_BEGIN = '-----BEGIN CHAINS PROOF OF FUNDS-----';
export const PROOF_SIGNATURES = '-----SIGNATURES-----';
export const PROOF_END = '-----END CHAINS PROOF OF FUNDS-----';

/** The text of a proof: the statement, then one line per address with its signature. */
export function formatProof(statement: string, signatures: { address: string; signature: string }[]): string {
  const lines = signatures.map((s) => `${s.address} ${s.signature}\n`).join('');
  return `${PROOF_BEGIN}\n${statement}\n${PROOF_SIGNATURES}\n${lines}${PROOF_END}\n`;
}

/** A statement as Qt makes it of the text typed: spaces simplified (QString::simplified). */
export function simplified(text: string): string {
  return text.replace(/\s+/g, ' ').trim();
}

export interface ParsedProof {
  statement: string;
  /** The signature lines: address and signature, or null for a line that has not two parts. */
  entries: ({ address: string; signature: string } | { invalid: string })[];
}

/** Take a proof apart; null when the text is not one. */
export function parseProof(text: string): ParsedProof | null {
  const lines = text.split('\n').filter((l) => l.length > 0);
  const begin = lines.indexOf(PROOF_BEGIN);
  const signatures = lines.indexOf(PROOF_SIGNATURES);
  const end = lines.indexOf(PROOF_END);
  if (begin < 0 || signatures !== begin + 2 || end <= signatures) return null;
  const entries: ParsedProof['entries'] = [];
  for (let i = signatures + 1; i < end; i++) {
    const parts = simplified(lines[i]).split(' ');
    entries.push(parts.length === 2 ? { address: parts[0], signature: parts[1] } : { invalid: parts[0] ?? '' });
  }
  return { statement: lines[begin + 1], entries };
}

/** The verdict of a check: valid, partly valid, or not valid, with the amount on the addresses now. */
export function proofVerdict(valid: number, invalid: number, total: string): string {
  if (invalid === 0 && valid > 0) return `VALID. The holder of ${valid} address(es), with ${total} CHN on them now, signed this statement:`;
  if (valid === 0) return 'NOT VALID. No signature of the proof is good. The statement was:';
  return `PARTLY VALID. ${valid} signature(s) are good, covering ${total} CHN now, and ${invalid} are not. The statement was:`;
}

// --- Multisig lounge (multisigdialog.cpp) ---

const COIN = 100_000_000n;

/** An amount typed as coins ("1.5"), as satoshis; null if it is not one (at most 8 decimals). */
export function coinsToUnits(text: string): bigint | null {
  if (!/^\d{1,8}(\.\d{1,8})?$/.test(text.trim())) return null;
  const [whole, frac = ''] = text.trim().split('.');
  return BigInt(whole) * COIN + BigInt(frac.padEnd(8, '0'));
}

/** Satoshis as coins with 8 decimals. */
export function unitsToCoins(value: bigint): string {
  const sign = value < 0n ? '-' : '';
  const v = value < 0n ? -value : value;
  return `${sign}${v / COIN}.${String(v % COIN).padStart(8, '0')}`;
}

/** An amount of an RPC answer (a JSON number of coins) as satoshis, exactly. */
export function jsonCoinsToUnits(amount: number): bigint {
  return coinsToUnits(amount.toFixed(8)) ?? 0n;
}

/** The descriptor of a shared address: the keys sorted by the descriptor, so all partners get the same. */
export function sharedDescriptor(required: number, keys: string[]): string {
  return `wsh(sortedmulti(${required},${keys.join(',')}))`;
}

/** A descriptor without its checksum. */
export function bareDescriptor(descriptor: string): string {
  return descriptor.split('#')[0];
}

/** "2 of 3" for the descriptor of a shared address. */
export function signersText(descriptor: string): string {
  const m = /multi\((\d+),/.exec(descriptor);
  const keys = (descriptor.match(/,/g) ?? []).length;
  return t('%1 of %2', m ? m[1] : '?', keys);
}

/** The key of a wallet address, with its derivation path, from the descriptor of getaddressinfo ("wpkh(KEY)#..."). */
export function keyOfDescriptor(desc: string): string {
  const m = /^wpkh\((.+)\)#/.exec(desc);
  return m ? m[1] : '';
}

interface DecodedPsbt {
  tx?: { vout: { value: number; scriptPubKey: { address?: string } }[] };
  outputs?: { amount?: number; script?: { address?: string } }[];
  inputs: { final_scriptwitness?: unknown; partial_signatures?: Record<string, string>; witness_script?: { asm: string } }[];
  fee?: number;
}

/** How far a shared spend is (MultisigDialog::updateSignatureCount): signatures, what it pays, the fee. */
export function psbtStatus(decoded: DecodedPsbt): string {
  // The input with the fewest signatures tells how far the transaction is.
  let signatures = -1;
  let needed = '?';
  let final = true;
  for (const input of decoded.inputs) {
    if (input.final_scriptwitness) continue;
    final = false;
    const count = input.partial_signatures ? Object.keys(input.partial_signatures).length : 0;
    if (signatures < 0 || count < signatures) signatures = count;
    if (input.witness_script) needed = /^(\d+) /.exec(input.witness_script.asm)?.[1] ?? needed;
  }
  // A PSBT of version 2 lists its outputs itself; an older one carries a transaction.
  const outputs = decoded.tx ? decoded.tx.vout.map((o) => ({ amount: o.value, address: o.scriptPubKey.address })) : (decoded.outputs ?? []).map((o) => ({ amount: o.amount ?? 0, address: o.script?.address }));
  const payments = outputs.map((o) => `${o.amount.toFixed(8)} CHN to ${o.address ?? '(no address)'}`);
  const fee = decoded.fee !== undefined ? decoded.fee.toFixed(8) : '?';
  return `${final ? 'Fully signed, ready to send.' : `Signatures: ${signatures} of the ${needed} needed.`} Pays ${payments.join(', and ')}. Fee: ${fee} CHN.`;
}

// --- Block explorer (blockexplorer.cpp) ---

export type ExplorerQuery = { kind: 'height'; height: number } | { kind: 'hash'; hash: string } | { kind: 'invalid' };

/** What a search of the explorer is: a height, a hash (block or transaction), or neither. */
export function explorerQuery(input: string): ExplorerQuery {
  const q = input.trim();
  if (/^[0-9]{1,9}$/.test(q)) return { kind: 'height', height: Number(q) };
  if (/^[0-9a-fA-F]{64}$/.test(q)) return { kind: 'hash', hash: q.toLowerCase() };
  return { kind: 'invalid' };
}

// --- Timestamps (timestampdialog.cpp) ---

/** What a stamp publishes: "STAMP" and the SHA-256 of the file, in hexadecimal. */
export function stampPayload(sha256: string): string {
  return '5354414d50' + sha256.toLowerCase();
}

// --- Third-party transaction URLs (TransactionView: strThirdPartyTxUrls) ---

/** The menu items for a transaction: one per web address with a host, its %s replaced by the id. */
export function thirdPartyLinks(setting: string, txid: string): { label: string; url: string }[] {
  const out: { label: string; url: string }[] = [];
  for (const raw of setting.split('|')) {
    const text = raw.trim();
    if (!text) continue;
    let url: URL;
    try {
      url = new URL(text.replace(/%s/g, 'TXID'));
    } catch {
      continue;
    }
    if (!url.host || (url.protocol !== 'https:' && url.protocol !== 'http:')) continue;
    out.push({ label: url.host, url: text.replace(/%s/g, txid) });
  }
  return out;
}
