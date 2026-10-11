# FastBlocks

FastBlocks is a sidechain of Chains (CHN) in slot 5. It is a hybrid chain:
*PoS blocks* (blocks of stake) every 30 seconds, signed by validators who stake
pegged CHN, and *BMM blocks*, the template's blind merged mined blocks, as the
fallback. The mainchain anchors the PoS chain through BMM commitments to
*tickets*. The full design is `~/skydoge/design/fastblocks.md`; this file says
what is built. The fee pot won through BMM bids is inspired by sol-drivechain;
no code of it is used.

Built so far: phase 2 of the design (a fixed validator set from the chain
parameters). Not yet: bonding and unbonding, epoch snapshots and a mainchain
seed, slashing and evidence (phase 3); wallet and GUI finality, the `evidence`
message, IBD from tickets (phase 4).

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
| An acknowledged height has a commitment on record; may come later | `pos-ack-unknown` |

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
