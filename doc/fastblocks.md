# FastBlocks

FastBlocks is a sidechain of Chains (CHN) in slot 5. It is a hybrid chain:
*PoS blocks* (blocks of stake) every 30 seconds, signed by validators who stake
pegged CHN, and *BMM blocks*, the template's blind merged mined blocks, as the
fallback. The mainchain anchors the PoS chain through BMM commitments to
*tickets*. The full design is `~/skydoge/design/fastblocks.md`; this file says
what is built. The fee pot won through BMM bids is inspired by sol-drivechain;
no code of it is used.

Built so far: phase 2 of the design (a fixed validator set from the chain
parameters) and phase 4 (RPCs, wallet and GUI finality, the staking page, the
`fbevidence` and `fbgettickets` messages, headers that wait, catching up from
tickets). Not yet: phase 3 (bonding and unbonding, epoch snapshots and a
mainchain seed, slashing): so no `createbond`/`createunbond` either, and evidence
is found, checked and relayed but not yet put in blocks nor punished.

## Identity

| | Main | Testnet | Signet | Regtest |
|---|---|---|---|---|
| Slot | 5 | 5 | 5 | `-sidechainslot` |
| P2P port | 9155 | 19155 | 39155 | 29155 |
| RPC port | 9154 | 19154 | 39154 | 29154 |
| Magic bytes | `66 62 6c 01` | `66 62 6c 02` | `66 62 6c 03` | `66 62 6c 04` |
| Address prefix | `fbl` | `tfbl` | `tfbl` | `rsc` |

Programs: `fastblocksd`, `fastblocks-cli`, `fastblocks-qt`; data directory
`~/.fastblocks`. The main network refuses to start until
`main_activation_height` names the mainchain block that activated slot 5; the
test network and signet only warn (it is 0 there until the slot activates).

## Parameters

| | Main | Testnet / Signet | Regtest |
|---|---|---|---|
| Slot | 30 s | 30 s | 1 s |
| Slots per epoch | 120 | 20 | 20 |
| Validators (min active / max) | 4 / 100 | 3 / 32 | 1 / 8 |
| `max_unanchored_slots` | 1200 (1 h) | 600 | 100 |
| `stall_seconds` | 1800 | 600 | 30 |
| Producer share | 50 % | 50 % | 50 % |
| Anchors that make a block final (wallets) | 6 | 3 | 1 |
| Validator set | none yet | none yet | `-posvalidators` |

Without enough validators a network has no PoS blocks: it is the template's
chain, merged mined only. Regtest takes its fixed set with
`-posvalidators=<x-only key hex>[:<stake>],...` (consensus: every node of a
test network needs the same list).

## Blocks

**PoS block**: an extended block of the template (`BLOCK_EXTENSION_BIT`). The
header carries `hashSections` and a BIP340 signature by the leader of the slot
over `TaggedHash("hpos/block", hash)`; the hash covers the 80 bytes and
`hashSections`, not the signature. In the 80 bytes: `nTime` is the start of
the slot, `nBits` the chain's proof-of-work limit (a constant), and `nNonce`
the mainchain height of the anchor the block acknowledges (0 if none). The
sections hold the acknowledgement, if any.

**Leader**: picked by stake among the validators sorted by key, from
`TaggedHash("hpos/leader", seed || slot) mod total`, with
`seed = TaggedHash("hpos/seed", epoch || genesis hash)`.

**Anchor ticket**: `{version 1, anchored block P, payee script}`; the
mainchain commits to `h* = TaggedHash("hpos/ticket", ticket)` through
`createbmmrequest`. Tickets travel in the `fbticket` P2P message.

**Acknowledgement**: `{ticket, main height H}` in a PoS block Q. Q takes the
peg actions there, as a BMM block does: the fee pot to the payee, deposits and
bundle outcomes up to H − 1, a bundle of the withdrawals at or below P with P
as its nonce, refunds, payouts.

**BMM block**: the template's. It anchors its parent, takes the peg actions,
and its first coinbase output's script is paid the pot. It carries
transactions only in stall mode.

## Rules (reject reasons)

Header of a PoS block (`ContextualCheckBlockHeader`):

| Rule | Reason |
|---|---|
| The network has enough validators | `bad-pos-inactive` |
| `nBits` is the proof-of-work limit | `bad-pos-bits` |
| `nTime` is the start of a slot after the first block | `bad-pos-time` |
| Its slot is after the slot of the last PoS block on its chain | `bad-pos-slot` |
| The slot has started (± `max_future_seconds`, 2 s); may come later | `pos-time-future` |
| Signed by the slot's leader | `bad-pos-sig` |
| Without an acknowledgement: less than `max_unanchored_slots` after the first block of stake no anchor covers, as far as the headers tell (the first one after the last BMM block, or the last acknowledgement) | `bad-pos-unanchored` |
| An acknowledged height has a commitment on record; may come later | `pos-ack-unknown` |

A header that may come later (`pos-time-future`, `pos-ack-unknown`, and
`bmm-unknown` for a BMM block) waits in the template's bounded set of waiting
headers, with the headers a peer sent after it; each is taken in once it and
every waiting header below it can be, in the order of their heights.

Block (`CheckBlock`):

| Rule | Reason |
|---|---|
| Only PoS blocks have a fee pot output | `bad-pos-pot-not-pos` |
| One pot output at most | `bad-pos-pot-multiple` |
| The sections read (an acknowledgement or nothing; no evidence yet) | `bad-pos-sections` |
| `nNonce` is the acknowledged height, 0 without one | `bad-pos-ack-nonce` |
| Ticket version 1 | `bad-pos-ticket-version` |
| The payee is a standard script of 80 bytes or less, not data | `bad-pos-ticket-payee` |

Acknowledgement (state):

| Rule | Reason |
|---|---|
| The mainchain committed to the ticket; may come later | `bmm-unknown` |
| ... first, at the height said | `bad-pos-ack-height` |
| After the last anchor's commitment | `bad-pos-ack-order` |
| P is known, and at or below the parent | `bad-pos-ack-anchored` |
| P is, or descends from, the last block that took the peg actions | `bad-pos-ack-chain` |

Every block (state):

| Rule | Reason |
|---|---|
| A PoS block's producer keeps at most 50 % of its fees: all the coinbase but the payouts and the pot ≤ 50 % of that and the pot | `bad-pos-producer-share` |
| A PoS block comes less than `max_unanchored_slots` after the first PoS block no anchor covers | `bad-pos-unanchored` |
| A BMM block carries transactions only when the mainchain block before its commitment is `stall_seconds` after the last PoS block | `bad-pos-bmm-not-stalled` |
| A refund request only in a block that takes the peg actions | `bad-sc-refund-not-peg` (template) |
| The rules run with a chainstate attached | `bad-pos-context` |

The state of these rules is one entry of the sidechain store (table `0x55`)
and the pot accrued by height (`0x56`): the store's journal is their undo.
Amounts are integers throughout.

## Finality

Not consensus: what nodes and wallets show (design §4.5). A block on the
active chain is

- **soft** while no anchor covers it (only blocks of stake are on it);
- **anchored** once an anchor covers it: it is at or below the block an anchor
  anchors (the block a ticket names, or a BMM block's parent). Undoing it takes
  the mainchain's merged miners, not stake;
- **final** once `final_anchors` anchors cover it: 6 on main, 3 on testnet
  and signet, 1 on regtest.

`getblock` gives a block's `finality` (`status`, `anchors`), and its `kind`
(`pos` or `bmm`), `slot`, `proposer` and `ack`; `getblockchaininfo` the
`anchored_tip` and the `final_tip`. In the wallet, `gettransaction` and
`listtransactions` give a confirmed transaction's `finality`, and
`getbalances` the confirmed coins by finality (`mine.finality.soft`,
`anchored`, `final`).

## Evidence

Two headers the leader of a slot signed for that slot prove that it signed
twice. A node notes the first header signed for each slot it sees (the last
4000 slots); a second one makes the proof. Proofs are checked, kept (1000, the
oldest slots go first) and relayed in `fbevidence`:

| Rule | Reason |
|---|---|
| Two headers of blocks of stake | `bad-pos-evidence-kind` |
| Of one slot, at its start | `bad-pos-evidence-slot` |
| Two different headers | `bad-pos-evidence-same` |
| Both signed by the slot's leader | `bad-pos-evidence-sig` |

`submitevidence <hex>` (the two headers, serialized) and `listevidence`.
Putting proofs in blocks and the penalty come with staking (phase 3).

## P2P

| Message | Content | Rule |
|---|---|---|
| `fbticket` | A ticket | Kept and relayed if it names a block the node knows, or the mainchain committed to it |
| `fbgettickets` | Up to 16 h* | The node sends the tickets it has; more than 16 is misbehaving |
| `fbevidence` | Two headers | Kept and relayed if it is a proof (above) |

**Catching up.** Headers and blocks come as usual and are checked in full
(leader signatures included); an acknowledgement waits until the node's own
record has the commitment it names. The tickets of the newest commitments, not
acknowledged yet, are asked for with `fbgettickets`: at each new mainchain
block, a node asks its peers for those of the commitments on record after the
last acknowledged one that it has neither a ticket nor a block of.

## RPCs

| RPC | |
|---|---|
| `getposinfo` | The validators, slot and leader, anchors, the pot, the producer |
| `getvalidators [epoch]` | The set, and the slots of the epoch each one leads |
| `getvalidator <pubkey>` | One validator: stake, slots, next slot, offences known |
| `getleaderschedule [epoch]` | The leader of each slot of an epoch |
| `getanchorinfo [count]` | The newest anchors, the anchored and final tips, the pot, the tickets known |
| `submitevidence <hex>`, `listevidence` | Evidence (above) |
| `setstaking true\|false`, `getstakinginfo` | This node as a validator: on or off (on at start), its keys, what it made, its next slot |

## The staking page

`fastblocks-qt` has a Staking page (Alt+6): the chain's slot and leader,
this node as a validator with buttons to start and stop staking, how far the
chain is anchored and final and the wallet's soft, anchored and final coins,
the validators of the epoch and the newest anchors.

## Fork choice

`nChainWork` is `anchors · 2^96 + (2^32 − 1 − last commitment height) · 2^64 +
height`: more anchors wins; with as many, the chain whose last anchor the
mainchain committed to first; then the longer. An anchor always wins over any
number of PoS blocks. Two chains with the same anchors and the same last
commitment share that anchor, so comparing heights is comparing PoS blocks
since it.

## Fees and the pot

A PoS block's producer keeps half its fees (output 0); the other half goes to
a pot output (`OP_RETURN 0xDE504F54`), which takes the coins out of
circulation into the state. An acknowledgement pays the pot accrued up to P
to the ticket's payee; a BMM block pays the pot accrued up to its parent to
its first output's script. Bidders bid for the pot on the mainchain, so its
value reaches the mainchain's miners.

## Producer and anchorer

- `-validatorkeyfile=<file>`: 32-byte secret keys in hex, one per line. At
  each slot one of them leads, the node builds a PoS block on its best chain,
  with the acknowledgement of the first committed ticket it can carry, notes
  the slot in `validator_protection.dat` (synced to disk before signing),
  signs and relays the block. It never signs at or below a slot it signed
  before, and refuses to start if another node holds the key file's lock.
- `-validatorpayout=<address>`: where the producer's share goes.
- With `setbmm`, while PoS blocks come, the node's merged miner makes a ticket
  per mainchain block (P is the block before its tip, or the last
  acknowledgement), relays it, and bids for its commitment (what the pot will
  pay, less 1 %); after `stall_seconds` without PoS blocks, it merged mines
  BMM blocks as the template does.
- A node keeps at most 1000 tickets; a flood from a peer pushes out the oldest,
  but not the node's own nor those the mainchain committed to. Until its
  committed ticket is acknowledged, the merged miner relays it again once per
  mainchain block.

`getposinfo` shows the validators, the slot and its leader, anchors, the pot
and the producer.

## Deviations from the design (phase 2)

- `nNonce` carries the acknowledged mainchain height and `nBits` the
  proof-of-work limit (the design had a proposer index and 0): the order of
  chains needs the height in the header, and Bitcoin's code divides by the
  work of `nBits`. The leader is known from the slot, so no index is needed.
- The fork-choice tie-break compares heights instead of PoS blocks since P:
  the same order (see above).
- The fee split is checked on an explicit pot output; fees the producer does
  not claim are lost, not added to the pot.
- `max_unanchored_slots` counts from the first PoS block no anchor covers,
  not from P: BMM block times do not move it.
- A slot is a PoS block's alone: BMM blocks do not take slots.
- The leader seed is from the epoch and the first block (phase 3 takes it
  from the mainchain).

## Deviations from the design (phase 4)

- Phase 4 came before phase 3: the wallet has no `createbond` and
  `createunbond` yet (a bond output without the rules that keep it would burn
  the coins), and evidence is relayed but not yet in blocks.
- Finality is shown on the Staking page (the chain and the wallet's coins), and
  by RPC per transaction; not yet as a badge per transaction in the transaction
  list, nor on the Overview page. `getbalance` has no `minfinality`.
- A BMM block counts as anchoring its parent, as in fork choice: the BMM
  block itself is soft until the next anchor.
- Evidence names no validator index (the leader is known from the slot); a
  proof of a slot outside the evidence window is still relayed (the window
  comes with phase 3).
- The design's header rule against `P_last` is checked as far as headers tell
  (an acknowledgement's P is in its block): the state checks the rest.
