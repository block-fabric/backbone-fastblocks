# FastBlocks

FastBlocks is a sidechain of Chains (CHN) in slot 5. It is a hybrid chain:
*PoS blocks* (blocks of stake) every 30 seconds, signed by validators who stake
pegged CHN, and *BMM blocks*, the template's blind merged mined blocks, as the
fallback. The mainchain anchors the PoS chain through BMM commitments to
*tickets*. The full design is `~/skydoge/design/fastblocks.md`; this file says
what is built. The fee pot won through BMM bids is inspired by sol-drivechain;
no code of it is used.

Built: phase 2 of the design (blocks of stake, anchors, the order of chains, the
fee pot), phase 3 (staking: bonds and unbonds, validator sets made at anchors, a
seed from the mainchain, jailing, slashing), phase 4 (RPCs, wallet and GUI
finality, the staking page, the `fbevidence` and `fbgettickets` messages,
headers that wait, catching up from tickets) and phase 5 (tests, fuzzing, the
audit).

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
| Slot | 30 s | 30 s | 1 s (`-posslotseconds`) |
| Slots per epoch | 120 | 20 | 20 |
| Validators (min active / max) | 4 / 100 | 3 / 32 | 1 / 8 |
| `max_unanchored_slots` | 120 (1 h) | 120 (1 h) | 100 |
| `stall_seconds` | 1800 | 600 | 30 |
| Producer share | 50 % | 50 % | 50 % |
| Anchors that make a block final (wallets) | 6 | 3 | 1 |
| Minimum stake | 1 CHN | 0.01 | 0.0001 |
| Unbonding (anchors) | 2880 (two days) | 120 | 20 |
| Window for proofs (anchors) | 1440 | 60 | 10 |
| Seed lookahead | 1200 s | 600 s | 3600 s (`-posseedlookahead`) |
| Penalty, reporter's share | 10 %, 1/8 | 10 %, 1/8 | 10 %, 1/8 |
| Jailing | under 50 % of 10 slots or more led: 1 epoch more | same | same |
| Reserve paid per anchor | 1/1440 | 1/60 | 1/5 |
| Validators to start with | none | none | `-posvalidators` (a bootstrap) |

Bitcoin's heuristics that count blocks by the target spacing (headers sync and
block download timeouts, stale tips, verification progress) count a block a
slot: `nPowTargetSpacing` is 30 s on every 30-second network (60 s before,
which gave initial block download half the headers timeout it needs).

Without enough validators a network has no PoS blocks: it is the template's
chain, merged mined only (and its BMM blocks carry the transactions). That is
how the main and test networks start: validators bond, and once a validator set
made at an anchor has `min_active_validators`, blocks of stake start from the
epoch after next; they stop again the same way if stake leaves (design 3.5,
"PoS on and off"). Regtest may also start with a fixed set,
`-posvalidators=<x-only key hex>[:<stake>],...` (consensus: every node of a
test network needs the same list): these genesis validators are in every set
until jailed or banned, with their stake as weight on top of what they bond.

## Blocks

**PoS block**: an extended block of the template (`BLOCK_EXTENSION_BIT`). The
header carries `hashSections` and a BIP340 signature by the leader of the slot
over `TaggedHash("hpos/block", hash || genesis hash)`; the hash covers the 80
bytes and `hashSections`, not the signature. (The genesis hash: the networks'
slots line up, and a header signed on one is no signature on another, nor half
of a proof that its signer signed twice.) The producer signs deterministically (BIP340
with all-zero auxiliary randomness): one block, one signature. In the 80 bytes: `nTime` is the start of
the slot, `nBits` the chain's proof-of-work limit (a constant), and `nNonce`
the mainchain height of the anchor the block acknowledges (0 if none). The
sections hold the acknowledgement, if any.

**Leader**: picked by stake among the validators of the slot's epoch sorted
by key, from `TaggedHash("hpos/leader", seed || slot) mod total`, with
`seed = TaggedHash("hpos/seed", epoch || h)`: `h` is the hash of the highest
mainchain block whose median time is at or before the epoch's start less
`seed_lookahead_seconds` (null if no block on record is that old). A node knows
it once its record has a later block (median times never go down); validators
have no say in it, and the stake of the epoch was fixed before it exists (see
Staking).

**Anchor ticket**: `{version 1, anchored block P, payee script}`; the
mainchain commits to `h* = TaggedHash("hpos/ticket", ticket)` through
`createbmmrequest`. Tickets travel in the `fbticket` P2P message.

**Acknowledgement**: `{ticket, main height H}` in a PoS block Q. Q takes the
peg actions there, as a BMM block does: the fee pot to the payee, deposits and
bundle outcomes up to H − 1, a bundle of the withdrawals at or below P with P
as its nonce and P's height as its lock time, refunds, payouts. The peg actions
count at P's height (template hook `PegStep::peg_height`): a bundle failure Q
sees is noted at P's height, and the retry delay counts from it. So every block
that acknowledges the same anchor builds the same bundle, whatever its own
height (design §4.3.4). P is at or after the block the last anchor anchored; it
may be below the block that took the last peg actions (a ticket made for the
next mainchain block before the last commitment was acknowledged): then Q builds
no bundle (no peg actions may come between P and Q for the bundle to be P's
alone), and the next anchor whose P is at or after that block does.

**BMM block**: the template's. It anchors its parent, takes the peg actions,
and its first coinbase output's script is paid the pot. It carries
transactions only in stall mode. Its coinbase may carry proofs (see Evidence).

## Rules (reject reasons)

Header of a PoS block (`ContextualCheckBlockHeader`):

| Rule | Reason |
|---|---|
| `nBits` is the proof-of-work limit | `bad-pos-bits` |
| `nTime` is the start of a slot after the first block | `bad-pos-time` |
| An acknowledgement (`nNonce`) names a commitment after the last anchor's of the chain below it (its chain key's): one anchor per commitment, in order | `bad-pos-ack-order` |
| The epoch's validator set can make blocks of stake | `bad-pos-inactive` |
| The record has the block that seeds the epoch; may come later (not a slot to come: below) | `pos-seed-unknown` |
| Signed by the slot's leader (checked before any rule that reads more of the chain, or lets the header wait; when the node does not know the epoch's set yet, the set of an anchor of a branch it never connected, when the block is connected) | `bad-pos-sig` |
| A header whose epoch's set the node does not know: the branch leaves the active chain at or above its final block, and the anchor that made the set is not on the active chain (the state pruned that one's set). Not punished | `bad-pos-fork-too-deep` |
| ... and, from a peer, it fits the budget of headers the node cannot check yet: the peer holds fewer than `unchecked_headers_per_peer` (10000; 2000 on regtest), and their stored blocks weigh at most `unchecked_block_bytes_per_peer` (32 MiB), whatever other peers hold; an inbound peer, `unchecked_headers_per_inbound_peer` (500; on regtest 2000, or `-posuncheckedinbound`) and `unchecked_block_bytes_per_inbound_peer` (8 MiB; 32 MiB on regtest), and only while fewer than `unchecked_inbound_holders` (8) other inbound peers hold any: see "Headers and blocks the node cannot check yet" (template; not consensus). Not punished; asked again once the tip moved | `unchecked-budget` |
| ... and, from a peer, the block index has fewer than two headers of its slot signed by its leader for good (the first, and the proof that it signed twice): a further one of that signer is spare, taken only with a header on top of it in the same message, or alone if it would be the best header or is the better tip an outbound peer announces, and then unchecked (its peer's, gone with it unless its block is connected); see "Headers of a slot" (template `HeaderSpare`, `SpareWanted`; not consensus). Not punished; asked again once the tip moved | `header-spare` |
| The slot has started (± `max_future_seconds`, 2 s); may come later if its leader signed it; one whose leader the node cannot tell (set or seed unknown) does not wait, nor is punished | `pos-time-future`, `pos-time-future-unchecked` |
| Its slot is after the slot of the last PoS block on its chain | `bad-pos-slot` |
| Without an acknowledgement: less than `max_unanchored_slots` after the first block of stake no anchor covers, as far as the headers tell (the first one after the last BMM block, or the last acknowledgement) | `bad-pos-unanchored` |
| An acknowledged height has a commitment on record; may come later | `pos-ack-unknown` |

While the network has validators, any block (a BMM block too) may be at most
10 minutes ahead of the node's clock (`time-too-new`, Bitcoin's rule with two
hours): BMM blocks far ahead would push the median time past, which a block of
stake's slot must come after, ahead. Without validators, two hours, as the
template. "Has validators": genesis validators, or the set the newest anchor of
the active chain made is active -- the state of the tip, so that a node that
restarts or reorgs agrees with the others.

**Headers the node cannot check yet.** A node that catches up has the headers
of the chain before its blocks: the set of an epoch is made by an anchor it has
not connected, and the signature of such a header is checked when its block is
(as is every block's). What such a header can do to the order of chains is
bounded: it counts one anchor at most, for a commitment on record after the last
anchor below it (`bad-pos-ack-order`); its branch has an anchor the node took in
checked (a signed acknowledgement, or a BMM block, which costs a commitment of
its own), since the sets of the first epochs after a fork are those of anchors
below it; and it does not take back a final block nor reach below the window of
sets the state keeps (`bad-pos-fork-too-deep`). How many such headers a node
holds, and what it does with their signatures and blocks, is the next section's
design: they belong to the peers that sent them (`unchecked-budget`), and a block
whose signature fails when connected is a failure of that copy, not of the block.

**Headers of a slot.** A header costs its leader a signature, nothing more: the
leader of a slot can sign as many headers of it as it likes (another merkle root
each), on the block before it, deep in the chain too, and once unbonded or banned
it has nothing left to lose. Each would be checked (the node knows the leader, so
no budget of unchecked headers applies), and the order of chains is not work (no
anti-DoS work threshold). So the block index keeps at most two headers of a slot
signed by one leader for good (`FastBlocksHooks::HeaderSpare`, counting the
entries the template says are there for good, `HeaderKept`: taken checked,
connected, or loaded at startup, each with its signer, as its signature was
checked then, or found by its signature when asked): the first, and a second, the
proof that it signed twice. Once a signer is proven so, a further header of the
slot from it, from a peer, is *spare* (template, doc/sidechain.md, "Spare
headers"); another leader's header of the slot (a branch whose set names
another, or a record that moved the epoch's seed) is not, and is taken as any:

- alone (a `headers` message of it alone, a block, a compact block): refused,
  `header-spare`, not punished. Its signature is not checked: it can change
  nothing (refused alone, or taken unchecked below, its block's signature checked
  when connected), and a spare header is in no block index, so nothing remembers
  it: sent again and again, each copy cost three signature checks (its own, and
  the two of the proof it made again; review 7,
  `attack7_spare_header_replayed_costs_signatures`: 500 copies, none now). Nor
  does it wait for its slot (one to come: `header-spare` too). The proof against
  its signer is the two headers kept, whose signatures were checked: put in the
  pool (Evidence) when the pool has none of the offence (after a restart, say).
  But not one the node may need now (template `SpareWanted`): one that would be
  its best header (a better key: more anchors, say), or the tip an outbound peer
  announces, of a chain better than the active one. The two first headers of a
  slot may be junk its leader signed (or anyone relays), and the block the
  network builds on is announced alone: it is taken unchecked (below) and its
  block fetched at once, not a slot later;
- with a header on top of it in the same message: taken unchecked, the peer's
  within its budget (see the next section), kept for good only once its block is
  connected. An honest node may have seen two other headers of the slot first and
  still needs the one the network builds on: the peer that sends it a block on
  that one answers its `getheaders` with the headers from the fork, the spare
  header and the block's on it.

The node's own blocks (and `-reindex`) are never spare. Memory and disk for
headers of a slot: two entries for good per leader of the slot (one a branch
whose set or seed names, so few), and what peers hold within their budget of
unchecked headers, gone with them. (Review 6: 2000 headers one leader signed for
one slot, seven blocks deep, were each a permanent entry of the block index;
`attack6_leader_signs_headers_without_bound`, `slot_spare_header_taken_on_a_branch`.
Its review, item 14: the two places were first come, so two junk headers held up
the honest block of the slot, or another leader's, announced alone;
`slot_junk_headers_then_the_announced_block`, `slot_headers_counted_by_signer`.)

A header that may come later (`pos-time-future`, `pos-ack-unknown`,
`pos-seed-unknown`, and `bmm-unknown` for a BMM block) waits in the template's bounded set of waiting
headers, with the headers a peer sent after it; each is taken in once it and
every waiting header below it can be, in the order of their heights. A waiting
header keeps the peer that sent it, whichever way it came (a headers message, a
compact block, a block), and is taken in within that peer's budget; one whose
sender is not known is never taken as the node's own (`UNKNOWN_SOURCE`): it is
taken in only if the node can check it. (Review of audit 6: blocks of stake on a
branch the node could not check, sent as `block` messages and acknowledging the
next mainchain height, were taken as the node's own once it was on record,
outside every budget, and never left the block index.)

Block (`CheckBlock`):

| Rule | Reason |
|---|---|
| Only PoS blocks have a fee pot output | `bad-pos-pot-not-pos` |
| One pot output at most | `bad-pos-pot-multiple` |
| The sections read (an acknowledgement or none, then 16 proofs at most; nothing is no bytes) | `bad-pos-sections` |
| Each proof has the form of one (see Evidence) | `bad-pos-evidence-kind`, `-slot`, `-same` |
| `nNonce` is the acknowledged height, 0 without one | `bad-pos-ack-nonce` |
| Ticket version 1 | `bad-pos-ticket-version` |
| The payee is a standard script of 80 bytes or less, not data | `bad-pos-ticket-payee` |
| For a header the node has, the block carries that header's signature and sections hash: the block hash does not cover the signature (template, `AcceptBlock`; a mutated block, the header stays valid; a compact block with another one is not reconstructed). Another valid signature of the slot's leader is the same block (template hook `CheckOtherExtension`): taken with the node's own signature, which it keeps, and its peer not punished; if the node's own is not valid (taken before it could tell the leader), the valid one replaces it; one the node cannot check yet (it cannot tell the leader) is the same block, taken with its header's signature (see "Headers and blocks the node cannot check yet") | `bad-blk-extension` |

Acknowledgement (state):

| Rule | Reason |
|---|---|
| The mainchain committed to the ticket; may come later | `bmm-unknown` |
| ... first, at the height said | `bad-pos-ack-height` |
| After the last anchor's commitment | `bad-pos-ack-order` |
| P is known, and at or below the parent | `bad-pos-ack-anchored` |
| P is, or descends from, the block the last anchor anchored (if P is below the block that took the last peg actions, Q builds no bundle) | `bad-pos-ack-chain` |

Every block (state):

| Rule | Reason |
|---|---|
| A PoS block is signed by the leader of its slot, as the chain below it and the record say when it is connected (its header may have come before the node knew the epoch's set; the record may have moved the seed since) | `bad-pos-sig`, `bad-pos-inactive`, `pos-seed-unknown`, `bad-pos-set-unknown` (a set the state no longer has) |
| The seeds the chain below a PoS block read (of the last block of stake's epoch, so of all: see Seed) are the record's: else a mainchain reorg moved them, and the blocks from the moved epoch are connected again first (a failure against the record, given back then; not the peer's fault) | `pos-seed-moved` |
| A PoS block's leader is not banned in the state of the chain below it: from the block after the one that banned it, a banned validator leads no more, and its slots stay empty | `bad-pos-leader-banned` |
| A PoS block's producer keeps at most 50 % of its fees: all the coinbase but the payouts and the pot ≤ 50 % of that and the pot | `bad-pos-producer-share` |
| A PoS block comes less than `max_unanchored_slots` after the first PoS block no anchor covers | `bad-pos-unanchored` |
| A BMM block carries transactions only when the mainchain block before its commitment is `stall_seconds` after the last PoS block | `bad-pos-bmm-not-stalled` |
| A refund request only in a block that takes the peg actions | `bad-sc-refund-not-peg` (template) |
| Bonds, unbonds and proofs | see Staking and Evidence |
| The rules run with a chainstate attached | `bad-pos-context` |

The state of these rules is one entry of the sidechain store (table `0x55`)
and the pot accrued by height (`0x56`); staking's is below. The store's journal
is their undo. Amounts are integers throughout.

## Headers and blocks the node cannot check yet

A header of a block of stake is checked against the leader of its slot, whom
the node knows only from the validator set of the slot's epoch, made by an
anchor of the header's own chain. When that anchor is on blocks the node has not
connected (a node that catches up, or a branch it never connected, such as the
other side of a partition), the node cannot check the signature. It cannot
refuse the header either: it would never catch up, nor ever leave a branch it
connected for one with more work whose sets it does not know. So it takes such a
header unchecked, and checks the signature when the block is connected, as it
does for every block. The block hash does not cover the signature: anybody can
send an honest header with a forged one, a forged copy of an honest block, or
any number of headers nobody can check. This section is the one design for that
(it replaces the patches of audit 6 and its review: the per-peer count of
`pos-unchecked-budget` with the hooks `BeginHeaders` and `SourceGone`, the other
extensions kept by block, `m_other_extensions`, the failed blocks asked for again,
`m_extension_reask` and `OtherExtensionMayFix`, and before them the message's
claim of work).

### The design

Words: a header is **checked** when the node verified its signature (it could
tell the leader when the header came, or its block was connected). A header is
**unchecked** when the node took it without verifying its signature (template
hook `HeaderCheckable`: FastBlocks' `LeaderOf` says `SET_UNKNOWN`), and so is
every header it takes on top of an unchecked one: unchecked headers are the
newest part of a branch, never below a checked one. A header stays unchecked
until its block is connected.

1. **Unchecked headers belong to the peers that sent them** (template,
   `ChainstateManager::m_unchecked`). Each unchecked header has a set of
   *holders*: the peers that sent it, in a headers message, a compact block or a
   block, and the peers that sent a header on top of it (a peer that sends a
   header holds the chain below it too). The accounting is by header: what a
   peer holds is the headers that have it as a holder.
2. **Bounds, by peer.** A peer holds at most `unchecked_headers_per_peer`
   headers (10000; 2000 on regtest), and the blocks the node stored of the
   headers it holds weigh at most `unchecked_block_bytes_per_peer` (32 MiB) --
   an outbound or manual peer. An inbound peer, which anyone can open, holds at
   most `unchecked_headers_per_inbound_peer` (500) and
   `unchecked_block_bytes_per_inbound_peer` (8 MiB, more than a block of the
   largest size), and only `unchecked_inbound_holders` (8) inbound peers hold
   any at once: a further one is refused until one of them holds none (nothing
   is dropped for it, nobody disconnected). The honest peer a node catches up
   from is usually one it chose. (Regtest: inbound peers as outbound ones, 2000
   headers and 32 MiB, since the test framework's peers are inbound;
   `-posuncheckedinbound` gives them less.) A header over either is refused (`unchecked-budget`,
   `BLOCK_HEADER_LOW_WORK`: not punished; the node asks the peer for its headers
   again once its tip moved, when its connected blocks gave room back). Every
   connected peer has this whole budget whatever the others hold or share: no
   header is ever dropped to make room for another peer's. There is no node-wide
   total: one smaller than the peers' budgets put together can always be taken
   from an honest peer (attackers that hold the same headers in pairs locked
   it, and new connections took headers from the peer that alone held the most:
   the adversarial review of the earlier design, which had
   `unchecked_headers_total` and eviction). Memory is bounded by the peers the
   node is connected to instead (below).
3. **Volatile.** An unchecked header is not written to the block index
   database, and leaves the block index as soon as no peer holds it (its last
   holder disconnected). Deleting from the block index is new to Bitcoin's code,
   where an entry, once made, lives as long as the node: only unchecked headers
   ever leave, any time `cs_main` is free, and a checked header stays so. So no
   code uses an unchecked header's entry once it released `cs_main` since it
   found it: each path that can reach one holds the lock while it uses it
   (`getblockheader`, `getchaintips`, `getblock` of such a block, the wallet's
   `findBlock`, what a block message reads of its parent, the block a producer
   acknowledges...), or looks it up again by hash once it has the lock again if
   any entry left meanwhile (`m_unchecked_erasures`: `ActivateBestChain`,
   `InvalidateBlock` and its map of candidates, the headers and compact block
   handlers; `PreciousBlock`, `InvalidateBlock` and the follower take a hash), or
   never sees unchecked headers at all (`LookupCheckedBlockIndex`: the RPCs and
   REST calls that read a block without the lock, `getblocktxn`, the wallet's
   coinbase roles, the kernel library); a low-work headers sync never starts on
   one. The peers' pointers into them (best known block, blocks in flight...) are
   moved to the nearest block that stays, under the same lock. The full list is
   the template's (doc/sidechain.md, "Pointers into the block index"; test
   `unchecked_erased_while_used`, where a peer leaves exactly where each path
   uses an entry). The entries taken out are freed a batch later (the last batch
   whole, and up to 4096 before it): defence in depth only, so that a path
   missed would read a dead entry rather than freed memory. A header whose block
   is connected is checked: it leaves the set, is written to disk, and is kept
   like any other. The node's own headers, and those read from disk or given by
   RPC, have the node as holder, which never goes.
   **Their blocks are stored apart.** The block of an unchecked header is stored
   in a file of its own (`blocks/unchecked/unc?????.dat`, template
   `BlockManager::WriteVolatileBlock`), never in the block files, if its sender
   holds the header and the block fits the sender's byte budget (else it is not
   stored; no fault of the peer's). It is charged to every holder of the header;
   a holder it does not fit stops holding that header and those on it. The file
   is deleted with the header (its number reused), and the block is written to
   the block files only once it passed every rule as it is connected (before
   its undo data). The directory is emptied when the node starts, and
   `-reindex` never sees these blocks.
4. **One extension a header.** The node keeps one signature for each header:
   the first that came, until it can tell the leader. Another valid one replaces
   it if the node's own is not valid (`CheckOtherExtension`, `REPLACE`); a block
   with another signature the node cannot check yet is the same block, stored
   with its header's signature (its peer becomes a holder). Nothing else is
   kept: no list of other extensions.
5. **A signature that fails is the copy's fault, not the block's.** A block whose
   header was unchecked and whose signature fails when it is connected
   (`bad-pos-sig`; template hook `ExtensionFailure`) is an *extension failure*:
   marked failed in memory only (the header is volatile), `BLOCK_TIME_FUTURE` for
   the peer that sent it, and for the headers on top of it (`extension-failed`,
   refused without punishment). Any other signature takes the failure back:
   `REPLACE` if the node can tell, or any other one if it cannot (the one it has
   is known bad). And the node asks for the headers again from the block's
   parent (getheaders), once, from every peer that holds the header or whose best
   chain has it: one pending request a peer (with one pending already, from the
   last block both share: the answer has the peer's copy of either). Nothing
   else is kept for a failure, so there is nothing an attacker can fill (the
   list of 64 failed blocks asked for again before could be filled: review of
   audit 6, item 8a; `extension_failure_asked_again_whatever_fails`, 300 failures
   of an attacker's own around an honest one). An honest peer answers with the
   leader's signature, which replaces the forged one, and the block is connected.
   The node never asks for a failed block again (`getdata`), nor keeps a list of
   such blocks.
6. **Fork choice is unchanged.** Unchecked headers count in the order of chains
   as before (bounded by `bad-pos-ack-order` and `bad-pos-fork-too-deep`), and may
   make the best header. Nothing that makes progress waits on the best header:
   blocks are fetched along each peer's own best chain, connected along the
   candidate with the most work whose blocks the node has, and initial block
   download ends by the tip. When the holders of an inflated best header go, it
   is deleted and the best header computed again.

### The invariants, and why each holds

Each against any number of peers, reconnections and new addresses.

- **Bounded memory and disk.** Unchecked headers in the block index: at most
  `unchecked_headers_per_peer` for each connected outbound or manual peer, and
  `unchecked_headers_per_inbound_peer` for each of the `unchecked_inbound_holders`
  inbound peers that may hold any, plus the node's own (every admission checks
  the sender's budget); their stored blocks, at most the byte budgets likewise
  (every block stored, and every header held, is charged to its holders and
  checked against their budgets; a holder over it stops holding). Outbound and
  manual peers are 19 at most, whatever `-maxconnections` (11 automatic ones,
  extras and feelers included, share `semOutbound`; 8 `-addnode` ones share
  `semAddnode`; `addnode onetry` and `-connect` are the operator's). So, with the
  main and test networks' parameters, at the very worst: 19 x 10000 + 8 x 500 =
  194,000 unchecked headers, at about 600 bytes each (block index entry and
  map node about 240, extension about 128, `m_unchecked` entry about 80, an
  `m_unchecked_held` node about 64 a holder) some 120 MB of memory, under
  190 MiB at a full 1 KiB each, plus the entries freed a batch late (at most
  10000 + 4096); and 19 x 32 MiB + 8 x 8 MiB = 672 MiB of blocks on disk. All of
  it unchecked, and gone with the peers (or at a restart). (Until 2026-10-13
  every peer had 10000 headers and 128 MiB, with no cap on inbound peers: with
  125 connections, 1.25 million entries and 16 GiB at worst; review of audit 6,
  item 10. `network_parameters` checks these figures.) Reconnecting changes nothing: the count is
  the number of headers and the bytes of their blocks, which leave with their
  last holder (the block's file deleted), so a new connection starts with no
  budget used and nothing left behind, on disk either: the same block sent
  again after a reconnection reuses the file of the one deleted. Other
  extensions: none are stored (4). Retry state: one pending getheaders a peer
  (5), and the template's ask-again-after-the-tip (2), both in the peer's state,
  gone with it. What may be dropped is only unchecked, and only by its own
  holders going (or by a holder that cannot afford the block of a header it
  holds): checked headers (the whole best chain the node checked), their
  signatures and their blocks are never deleted nor replaced, and no peer's
  headers are ever dropped for another's.
- **No stall.** (a) Checked headers need no budget: a node that catches up takes
  the honest chain as far as it can check (the sets are known an epoch and more
  ahead of the connected blocks), connects it, and can check further; whatever
  fills the budget, it progresses an epoch at a time, and unchecked headers only
  let it fetch ahead. (b) A branch whose sets the node never had (the other side
  of a partition) needs the honest peer's unchecked headers: the honest peer has
  its own budget, which attackers cannot use (each peer's own), take (there is
  no node-wide total, nor eviction) nor drop (a header is deleted only
  without holders, and the honest peer holds what it sent). (c) A forged
  signature on an honest header fails as an extension failure (5), not the
  block; the honest peers are asked for the header, whose valid signature
  replaces the forged one. (d) A forged copy of an honest block: refused as a
  mutated block if the node can tell (the hash stays valid); taken with the
  header's signature if it cannot (then (c)). (e) Floods of siblings and padded
  messages fill the sender's budget only; headers that claim more work than the
  honest chain and whose blocks the sender withholds: the node asks the sender
  for them, the block download times it out and disconnects it, and its headers
  go with it; with blocks, the node tries them, the signature fails (5), and they
  stay with their holders until those go. (f) A branch longer than a peer's
  budget beyond what the node can check syncs in windows: once the part the
  node holds leads, the node connects it, can check what follows, and the
  budget frees as it connects; it asks the peer for its headers again once its
  tip moved, and takes the next window (`feature_fastblocks_window.py`: a
  partition branch several times the budget, from one outbound peer, inbound
  attackers holding their whole budgets). Limit: the part before the branch
  leads must fit (below).
- **The limit of a long branch, and what the operator does.** A branch syncs in
  windows once the part the node holds leads (f). Before it leads, the node
  connects none of it (it connects only toward more work, and its own chain has
  more), so every header of the branch up to the first that leads, past the
  first epochs (whose sets are those of anchors below the fork, which the node
  can check), must fit one peer's budget: 10000 headers and 32 MiB of blocks
  from an outbound peer (500 and 8 MiB from an inbound one). That first header
  that leads has one anchor more than the node's own chain since the fork. So
  the limit is reached only when the node's own side kept anchoring too: the
  node takes such headers only on a branch that leaves its chain at or above its
  final block (`bad-pos-fork-too-deep`), which its side's anchors, net of the
  commitments it skipped (the other side's), have not covered `final_anchors`
  times; with an anchor in every mainchain block (about 20 slots apart), the
  other side leads within 10000 slots as long as the node's side made fewer
  than about 500 anchors since the fork: a partition of three days and more in
  which both sides anchored about as often, or one whose blocks past the first
  epochs weigh more than 32 MiB before the branch leads (five full blocks). The
  node cannot take more without a new denial of service: before the branch
  leads it cannot tell it from an attacker's, since a header it cannot check
  counts an anchor for any commitment on record after the last one below it
  (headers nobody can check claim as many anchors as there were commitments:
  the inflated best header), and holding headers or blocks past the budget on
  such a claim is the unbounded memory and disk the budget prevents. A safe way
  would be a presync like Bitcoin's low-work headers sync: take the peer's
  headers past its budget without storing them, check what needs no state of
  the branch (links, slots, times, the order of acknowledgements, and each
  anchor's ticket, which names a block of the branch, so that only commitments
  won on the mainchain make anchors), keep a commitment to them, and store them
  past the budget only once that work leads, downloading them again. Not built:
  it needs the tickets of the other side's commitments and a presync of the
  chain key. Until then, an operator whose node does not follow (`getchaintips`
  shows the other branch, `headers-only`, with more work than the active tip;
  with `-debug=validation`, its headers refused `over the budget
  (unchecked-budget)`):
  1. finds the fork: the height of the other branch's tip less its `branchlen`;
  2. `invalidateblock` the first block of its own chain after the fork: its tip
     goes back to the fork, the other branch leads at once, and the node takes it
     as one that catches up does, an epoch at a time, checking each epoch's
     headers once it connected the anchor that made its set (no budget needed);
     its own side's transactions go back to its mempool;
  3. once it is at the other side's tip, `reconsiderblock` the same block: its
     own branch, with fewer anchors, stays a fork.
  The same steps take a node past `bad-pos-fork-too-deep`, whatever its budgets,
  when its side made more anchors since the fork than the window of sets its
  state keeps (`evidence_window_anchors`: 1440 on main, 10 on regtest) or than
  its final block allows: by design, it then takes no header it cannot check on
  the other branch. (`feature_fastblocks_window.py` does both: on a node whose
  side anchored after the fork, with budgets too small for the branch before it
  leads, and on that side's validator when it is past the window of sets.)
- **No punishment of honest peers.** A header refused for the budget is
  `BLOCK_HEADER_LOW_WORK`; one off a branch too deep, too (`bad-pos-fork-too-deep`);
  an extension failure is `BLOCK_TIME_FUTURE` for the block's sender and for every
  header or block on it (5); a known header with another signature is never a
  fault. A peer is punished for a signature only when the node could check it as
  it came (a bad signature, or a mutated block): an honest peer sends only headers
  and blocks it connected, whose signatures it checked with the same chain (a
  record that differs is a failure against the record, pardoned as before). The
  node no longer asks for failed blocks, so no peer is asked for one.
- **No consensus divergence.** The rules of a block are unchanged and checked
  when it is connected, against its own chain's state. What this design decides
  is only whether and when the node holds a header and which copy of a signature
  it tries first, as for a block it does not have yet: the block whose valid
  signature exists connects on every node that gets it. An extension failure is
  never final (in memory, taken back by any other signature); unchecked headers
  are not written to the block index database (nor their blocks to the block
  files), so a restart starts with none and takes them again
  from peers; nothing read from memory (the sets read before) is a rule of a
  block.
- **Fork choice.** Unchecked headers can make the best header, but nothing waits
  on it (6): the honest peers' chains are fetched and connected whatever it is,
  and it goes with its holders.

FastBlocks' part: `HeaderCheckable` (the leader of the slot is `SET_UNKNOWN`),
`UncheckedLimits` (`unchecked_headers_per_peer`, `unchecked_block_bytes_per_peer`,
the inbound ones and `unchecked_inbound_holders`),
`ExtensionFailure` (`bad-pos-sig`), and `CheckOtherExtension`. The template's is
the rest, off unless a chain's `HeaderCheckable` says no. Tests: the template's
`sidechain_hooks_tests.cpp` (`unchecked_*`, among them
`unchecked_no_peer_drops_another`, `unchecked_inbound_budget_and_holders` and
`unchecked_blocks_stored_apart`,
`block_extension_unknown_taken`, `extension_failure_not_the_peers_fault`),
`fastblocks_tests.cpp` (`unchecked_siblings_not_taken`,
`unchecked_headers_checked_give_room`, `forged_signature_replaced`, and the
adversarial review's `attack_unchecked_budget_locked_by_shared_headers`,
`attack_unchecked_sybils_drop_honest_headers`,
`attack_unchecked_blocks_fill_the_disk`,
`attack_inflated_best_header_does_not_stop_the_chain`),
`feature_fastblocks_window.py` (a partition branch several times the budget,
synced in windows; and the operator's way past the limit), `unchecked_erased_while_used`
and `extension_failure_asked_again_whatever_fails` (template),
`feature_fastblocks_unchecked.py` (reconnection loops, many peers, each with
its own budget, forged copies, an inflated best
header while the node catches up, no honest peer punished; more inbound
attackers than may hold any, each within the inbound budget, while honest
outbound peers give the node a stretch several inbound budgets long), and the fuzz
targets `sidechain_unchecked` and `pos_state` (peers that come and go).
`feature_fastblocks_fork.py` keeps the attacker connected (its forged header would
otherwise go with it) and sees the forged signature replaced.

## Finality

Not consensus: what nodes and wallets show (design §4.5). A block on the
active chain is

- **soft** while no anchor covers it (only blocks of stake are on it);
- **anchored** once an anchor covers it: it is at or below the block an anchor
  anchors (the block a ticket names, or a BMM block's parent). Undoing it takes
  the mainchain's merged miners, not stake;
- **final** once `final_anchors` anchors cover it: 6 on main, 3 on testnet
  and signet, 1 on regtest (`-posfinalanchors=<n>` there, not consensus, so that
  a test sees the anchored state).

Anchors are counted net of the commitments the chain skipped. With as many
anchors, the chain whose last commitment came first wins: a commitment on
record that this chain did not acknowledge (the ticket of another branch, or
one whose ticket nobody showed: a bidder can keep a branch and its ticket to
itself) may be the anchor of a chain that leaves a block out. So a block counts
k anchors when every chain that forks below it has k fewer at most, taking
every skipped commitment since the last anchor they share as that chain's (the
newest 4 · `final_anchors` + 8 anchors are looked at). A skipped commitment
whose ticket or BMM block the node knows counts only against the forks that
could have it: where the block it rests on leaves the chain. Without skipped
commitments this is the number of anchors that cover the block. Finality is
what a node knows: one that never saw a ticket counts its commitment against
every fork.

`getblock` gives a block's `finality` (`status`, `anchors`), and its `kind`
(`pos` or `bmm`), `slot`, `proposer` and `ack`, and for a merged mined block the
`mainheight` of the mainchain block that committed to it; `getblockchaininfo` the
`anchored_tip` and the `final_tip`. In the wallet, `gettransaction` and
`listtransactions` give a confirmed transaction's `finality`, and
`getbalances` the confirmed coins by finality (`mine.finality.soft`,
`anchored`, `final`). The wallet counts the anchors of its own chain, the one
that ends at its last block (`interfaces::Chain::getAnchors(block)`, once per
call): during a reorg the node's tip moves first, and the anchors of the new
branch must not show a payment the reorg took out as anchored or final.

## Staking

Validators bond pegged CHN to a key, take it back after an unbonding delay, are
left out of the validator sets for a while when they miss their slots, and lose
part of their stake for good when they sign two blocks for a slot.

**Bond** (`createbond`): an output `OP_RETURN 0xDB'STK' | validator x-only key
(32) | owner key hash (20) | proof of possession (64) [| owner signature (65)]`
whose value is bonded:
it leaves circulation into the state, as a withdrawal does. The proof is the
validator key's BIP340 signature of `TaggedHash("hpos/bond", owner ||
genesis hash)`: nobody bonds a key not theirs (`signbondpop` on the
validator's node makes it; `createbond` asks the node's block producer if it
has the key). The first bond names the owner, who alone may unbond, and who is
paid what is unbonded (P2WPKH); and who alone adds bonds after. Every bond, the
first one too, carries the owner's signed message `FastBlocks bond <key>
<amount> <txid>:<n> <genesis>`, naming the first coin its transaction spends (a
coin is spent once, so the signature serves once). The proof of possession is
public: without this, anyone could top up a bond by a satoshi, and leave a
remainder under the minimum stake to an unbond its owner signed; or copy the
proof of a pending first bond into a bond of a satoshi of its own, mined first,
which the owner's would then need a signature for.

**Unbond** (`createunbond`): an output of value 0, `OP_RETURN 0xDC'UNB' |
validator key | amount (8) | sequence (4) | signature (65)`, the owner's signed
message `FastBlocks unbond <key> <amount> <sequence> <genesis>`; an amount of 0
(`all` in the message) unbonds all that is bonded when the unbond is applied.
The sequence is the validator's next: an unbond counts once, and each top-up of
the bond moves it on, so that an unbond signed before a top-up (and not mined:
dropped from the mempool, say; it is public) is void -- adding to a bond cancels
the unbonds pending. It takes the
amount off the stake at once, and pays it to the owner at the peg step of the
anchor `unbonding_anchors` after (the coinbase's payouts, as refunds); until
then a proof can still take a share of it. If the mainchain stalls, so does
unbonding.

| Rule (a transaction; in blocks and the mempool, template hook `ApplyTx`) | Reason |
|---|---|
| A bond output reads | `bad-pos-bond` |
| Its value is positive (and the total within MAX_MONEY) | `bad-pos-bond-amount` |
| It names an owner, the validator's own if it has one | `bad-pos-bond-owner` |
| The owner it names signed it (its amount and the transaction's first input), the first bond too | `bad-pos-bond-owner-sig` |
| Its proof of possession verifies | `bad-pos-bond-pop` |
| The key is not banned | `bad-pos-bond-tombstoned` |
| An unbond output reads | `bad-pos-unbond` |
| Its value is 0 | `bad-pos-unbond-value` |
| The validator has an owner | `bad-pos-unbond-unknown` |
| Its sequence is the validator's next (an unbond counts once; a top-up moves it on) | `bad-pos-unbond-sequence` |
| Its amount (all that is bonded, for 0) is positive, at most what is bonded | `bad-pos-unbond-amount` |
| What stays bonded is nothing or at least `min_stake` | `bad-pos-unbond-remainder` |
| The owner signed it | `bad-pos-unbond-sig` |

A smaller bond than `min_stake` is taken, but the key takes part only with
`min_stake` bonded (a genesis validator whatever it bonds).

**In the mempool.** A bond or an unbond is checked against the tip when it
enters the mempool, and again when a block changed the record of a validator it
names (template hooks `HasTxRules`, `TxRuleKeys`: those records alone decide):
one no block would take any more leaves the mempool. At most 500 a change of
the tip, the rest at the next ones. Rechecking them all at every block, under
`cs_main`, cost an ECDSA recovery and a BIP340 check per bond in the mempool,
and bonds of a satoshi are valid: 104 ms for 2000, about 11 s a block for a
mempool of 300 MB of them (review 5 of audit 6, attack 2,
`attack5_mempool_staking_recheck_per_block`).

**Validator sets.** Each anchor (the block that takes the peg actions: an
acknowledgement or a BMM block) makes a set from the state as it is then: the
eligible validators (not banned, enough stake, not jailed), the
`max_validators` largest (ties to the lower key), weight = bonded + genesis
stake. The set of epoch e is the one the newest anchor on the chain whose slot
is before the start of epoch e − 1 made -- an anchor's slot being that of its
time, or of the median time of the mainchain block that committed to it if that
is later (a block's time can be set back as far as the median time of the chain
before it, which a merged miner that makes every block after a stall controls:
it could otherwise fix the set of an epoch whose seed is known): the stake
is fixed at least an epoch before the epoch's seed exists, and a bond counts
from the epoch after the next anchor's next. The set has blocks of stake if it
has `min_active_validators`. Sets are kept by version, with each anchor's
(table `0x5a`, `0x5b`, `0x5c`), and pruned once no block to come can need them
(older than the window for proofs, and followed by an anchor before the epoch
before the current one). A node keeps the sets it read in memory too (4096): a
header of a branch whose anchor's set it never had is taken (within the bounds
of "Headers the node cannot check yet"), and its leader checked when its block
is connected.

**Seed.** See Leader above. A block of stake waits for its epoch's seed
(`pos-seed-unknown`). A mainchain reorg that moves the seed of an epoch takes
the blocks of stake whose leader changed back (as a lost commitment does), and
gives them back when the record does: the follower checks the leaders of the
newest blocks of the active chain at each new mainchain tip. The blocks of
stake of an epoch whose seed moved are connected again from its start, those
whose leader did not change too: the leaders they charged for empty slots (and
so the jailing, and the sets made after) are the new seed's. Which seeds moved
is told by the chain's state: each block of stake notes the seeds it read (of
its epoch, and of the slots it charged; table `0x5e`, every epoch: one entry an
epoch with blocks of stake), and the check compares them with the record's,
newest first, down to the first that has not moved: the seeds before it have
not either (its seed block is the same block, so the record is the same up to
it, and an earlier epoch's seed block is at or below it, told by that same
part of the record). So the check reaches as deep as the reorg, and costs one
look a mainchain tip when nothing moved. For that, the seeds of a state are
those of one record: a block of stake that comes after a reorg moved the seeds
the chain below it read, before the check, waits (`pos-seed-moved`, a failure
against the record) and is connected with the blocks the check connects again
(`seed_moved_then_a_block_before_the_check`). (Until review 7, only the last
three epochs were kept: a reorg reaching an older seed, about 40 minutes back on
testnet, went unseen by running nodes, while a node that takes the chain in
afresh read the new seed: two states of the epoch, its jailing and later sets;
`attack7_seed_moved_beyond_the_kept_epochs`.) So a node that restarts, or
whose record moved while it was down, finds the same as one that saw the reorg
come (a copy in memory of an earlier check, as before, saw no change after a
restart, and kept the old seed's state: review of audit 6, the check of seed
timing; `seed_moved_while_the_node_was_down`). The rules of a
block read the sets of anchors from its chain's state alone; the sets a node
keeps in memory serve headers only.

**Jailing.** Each block of stake notes that its leader made one, and that the
leaders of the slots since the last block of stake (an epoch of them at most)
made none (table `0x5d`, by epoch) -- of the slots at which a block could be
made: not those `max_unanchored_slots` or more after the first block of stake
no anchor covered (blocks of stake had stopped), nor those up to a BMM block the
block follows (which ends such a stop, or a stall). At an anchor, a validator
that made less than `liveness_min_bps` of the slots it led in an earlier epoch,
having led `liveness_min_assigned` or more, is jailed: left out of the sets
made in the next `jail_epochs` + 1 epochs. No coins are lost.
Two epochs per anchor: the epochs whose liveness it counts (and the sets it
may prune) are those before its *earliest* epoch, which is its own for a
block of stake and, for a BMM block, two hours before its time (it may be that
far ahead of the clock): only epochs surely over. The set it makes is made as
of its *set epoch*: the later of that earliest epoch and the epoch of the
median time of the mainchain block that committed to it, which whoever makes
the block cannot move ahead and which is not ahead of the clock. A jail ends
`jail_epochs` + 1 epochs after the set epoch, and a validator whose jail is over
by the set epoch is in the set. (Until 2026-10-13 the set was made as of the
earliest epoch: after blocks of stake stopped, the BMM blocks' sets left a
validator whose jail was over out for two hours more, and with a small set
blocks of stake could not start again: review of audit 6, item 9;
`jailed_validator_back_after_pos_stops`.)

**Slashing.** A block (of stake, or merged mined) may carry proofs that leaders signed two blocks
for a slot (below). The offender loses `slash_bps` (10 %) of its stake, bonded
then unbonding, once: it is banned for good, a proof of another of its offences
takes nothing more (`bad-pos-evidence-banned`), and the rest of its bond is
unbonded. From the next block on it leads no more: the sets made before its
ban still name it (for two epochs at most), but its slots stay empty
(`bad-pos-leader-banned`; its node's producer makes no block, and
`getleaderschedule` shows the slot as `banned`). The slots are not given to the
others: whoever carries a proof would otherwise choose the leaders of the slots
to come, and the state below a block (which every node of its chain has, as it
connects the block) says who is banned, so the rule is the same on every node.
(A header is taken before its block's state is known; its block is refused
when connected.) As for the reward: the
block that carried the proof is owed 1/`reporter_share_divisor` (1/8) of the
penalty, paid to the script of its coinbase's first output at the next peg
step (table `0x54`), unless its leader is the offender (a proof against
itself): then nothing, its share to the reserve too; the rest goes to a reserve that each anchor pays
1/`pot_reserve_anchors` of (rounded up) with the pot, to the anchor's payee.

**Peg actions of an anchor** (before the template's, `BeforePegStep`): the pot
and the share of the reserve to the payee; unbonds due and rewards owed; the
liveness of earlier epochs (jailing); the set of the anchor.

Tables: `0x50` the totals (bonded, unbonding, reserve, rewards owed, the set
version), `0x53` offences punished, `0x54` rewards owed, `0x57` validators
(owner, bonded, genesis stake, unbonds, sequence, jail, ban), `0x58` unbonds by
the anchor they are paid at, `0x59` eligible validators by weight, `0x5a`–`0x5c`
sets, `0x5d` liveness. (`0x51` and `0x52` are the template's `Q` and `R`.)

## Evidence

Two headers the leader of a slot signed for that slot prove that it signed
twice. A node notes the first header each leader signed for each slot it sees
(by slot and signer: two branches, or two records, may give a slot two leaders;
the last 4000); a second one of the same signer makes the proof. proofs are checked, kept (1000, the
oldest slots go first), relayed in `fbevidence`, and put in its blocks of stake
by a leader (16 at most, in the sections) while the chain has not punished
them, and in its BMM blocks by a merged miner (4 at most from the node, 16 by
the rules: one coinbase output each, `OP_RETURN 0xDD'EVD' | the proof`, after
the payouts). The same rules apply to both; a BMM block's reward goes to its
payee, the script of its coinbase's first output:

| Rule | Reason |
|---|---|
| In a BMM block's coinbase only (a block of stake has its proofs in its sections) | `bad-pos-evidence-not-bmm` |
| A coinbase output with the tag reads as a proof; 16 at most in a BMM block | `bad-pos-evidence-output`, `bad-pos-evidence-count` |
| Two headers of blocks of stake | `bad-pos-evidence-kind` |
| Of one slot, at its start | `bad-pos-evidence-slot` |
| Two different headers | `bad-pos-evidence-same` |
| Both signed by the slot's leader (on the chain below the block that carries it; for relay, the chain below either header or the active one) | `bad-pos-evidence-sig` |
| In a block: of a slot not after the block's | `bad-pos-evidence-late` |
| In a block: at most `evidence_window_anchors` anchors after the anchor that made the set of the offence's epoch (and a set the state still has) | `bad-pos-evidence-old` |
| In a block: not punished before (one per key and slot) | `bad-pos-evidence-known` |
| In a block: its validator is not banned (slashed once; so one proof per validator in a block) | `bad-pos-evidence-banned` |

`submitevidence <hex>` (the two headers, serialized) and `listevidence`. A
proof whose leader a node cannot tell yet (`pos-evidence-unknown`) is dropped
without blaming the peer. One the node keeps already is found before any
signature is checked, and dropped. Each leader found (below either header, or
at the tip) is checked once.

The set of the offence's epoch is looked for down the chain from the parent
(of the block that carries the proof, of either header, or the tip), and never
past `evidence_window_anchors` + 1 anchors (`SetFor`, `EvidenceWithin`): a set
further down is out of the window (each anchor counts one in the state, so the
anchors since it are more than the window: `bad-pos-evidence-old`, as when it
is found), and a proof of any old slot anyone can name, slot 1 say, made the
node walk the whole chain under `cs_main`, unblamed, 64 times a connection (154
ms for 32 proofs over 200,000 blocks; review 5 of audit 6, attack 1,
`attack5_old_slot_evidence_walks_the_chain`). The same bound holds for a block
whose acknowledgement `CheckBlock` notes (a slot far below its parent's, which
the header rules turn away: noted when connected otherwise). A header's own
slot is after the median time of its parent's chain (`time-too-old`), so its
leader's walk is short: the hooks that read its leader before that check
(`HeaderCheckable`, `HeaderSpare`) look at the time first too. (Until review 7
they did not: a header of slot 1 on a chain of 200,000 cost a walk of the whole
chain under `cs_main` before it was turned away, one a connection;
`attack7_early_slot_header_walks_the_chain`: 200,001 entries, none now.)

From a peer, only a proof whose form is wrong (`-kind`, `-slot`, `-same`: any node
sees it, whatever chain it has) is the peer's fault. One whose signatures do not
check against the leader this node finds (`bad-pos-evidence-sig`) is dropped
and not blamed: that leader depends on the chain a node has. A new node has an
inactive set at its tip (the main and test networks start with no validators)
and none of the headers' parents, and a node on another branch may find another
leader, for a true proof an honest peer checked (review 4 of audit 6, attack
2). So that a peer cannot have the node check signatures without end, the node
checks those of 64 proofs from a peer at once, then 2 a second (a token bucket
per peer, for 1024 peers at most): past that rate, a proof is dropped unchecked,
and the peer is not blamed for it either (`evidence_relay_rate`). A proof the
node keeps already costs no check, and is not counted.

The result of each signature check of a proof is kept, by proof and leader (the
last 4000; a proof the pool keeps counts as checked against its offender, and
that good result stays as long as the pool keeps the proof: no flood of checks
pushes it out), so that no proof is checked twice against one leader. The blocks
a node makes (of stake, and merged mined) read the pool under `cs_main`: the
cheap checks come first, by the offence a proof is kept under (its signatures
were checked: on any chain where the proof is one, it is that offence):
punished already, its validator banned, a slot after the block's. Then, by
value, the oldest slot first (the nearest the end of its window), those that
cost no signature check: a result kept, or a set of their slot this chain does
not have, or out of the window. Then the signatures of 32 proofs not checked
before at most per block, round robin in the pool's order from where the last
block left off: each is checked within 32 blocks (`EVIDENCE_CHECK_ROUNDS`, a
full pool of 1000), however many others a flood keeps unchecked. With a pool of
1000 proofs against a banned validator, leaving them out of a merged mined block
took 105 to 167 ms, and about 2 ms after; it checks no signature
(`evidence_pool_of_a_banned_validator`, `evidence_sig_checks_bounded`). Before
the pinned results and the round robin, a pool kept full of costly but useless
proofs (signatures that fail against the leader here) and a flood of checks
between blocks (relayed proofs, from many peers) kept a true proof from ever
being checked, or had its good result pushed out between its check and the block
(audit 6 review, item 11): with 1000 such proofs, its offender was never
slashed; now at the first block (`evidence_true_proof_among_a_flood`). In a
block's own check the signatures still come before `-known` and `-banned`: those
read the state alone (`RecordIndependent`), and a proof is only one once its
signatures say whose.

The pool is bounded and takes each proof once at most: full (1000), a proof of
a slot not after the oldest kept is not taken, nor relayed (it would be the one
pushed out at once, and new again the next time it came: a validator that
signed twice at 1001 slots, or unbonded, had nodes in a cycle relay it round for
ever, each time logged; review 4 of audit 6, attack 1). Proofs only ever leave
by being pushed out, so that floor only rises.

## P2P

| Message | Content | Rule |
|---|---|---|
| `fbticket` | A ticket | Kept and relayed if the mainchain committed to it, or if it names a block the node knows: of those, per mainchain block, 8 at most from the peers of one network group (a peer that connects again, with a new id, is the same group); 32 at most from all peers whose addresses are not routable together (local, as Tor's inbound connections are, or private: one group, as Bitcoin Core's for onion and local addresses, sized for honest relay, a bidder making one ticket a mainchain block); 8 a peer and 32 in all from peers the node cannot find (gone meanwhile: anyone); and 64 from all, first come, but past that a group's first one still (its fair share: a few groups that came first do not crowd an honest bidder's relayed ticket out), up to 128. A ticket the node has already is not counted (honest peers relay each new ticket to one another). Anyone can make them by the thousand, and each one taken goes whole to every peer: one attacker that reconnected 150 times in a mainchain block had 1200 taken and an honest one pushed out (review 5 of audit 6, attack 3); counted by peer for addresses that are not routable, 256 over Tor (audit 6 review, item 13; `tickets_reconnect_flood_over_tor_bounded`: 32 now); with the cap first come, a flood from 32 groups left an honest relayed ticket out (item 12; `tickets_honest_relay_under_a_flood`: taken, and acknowledged in the first slot after its commitment). A bidder's ticket dropped comes when the mainchain commits to it (catching up, below) |
| `fbgettickets` | Up to 16 h* | The node sends the tickets it has; more than 16 is misbehaving |
| `fbevidence` | Two headers | Kept and relayed if it is a proof (above) the node takes (new, and not older than its full pool); a peer that sends one whose form is wrong misbehaves (not one whose signatures do not check against the leader this node finds: that depends on its chain); past 64 proofs checked at once, 2 a second, a peer's proofs are dropped unchecked, not blamed; the set of an old slot is not looked for further down than the window (Evidence) |

A compact block for a header the node has, with another signature, is not
reconstructed, and not checked either (template): checking it against the
slot's leader cost a signature check under `cs_main` for every 200 bytes a peer
sends, of the tip's header say, unpunished (review 5 of audit 6, attack 4,
`attack5_compact_block_forged_extension_unpunished`). The block comes whole, and
`AcceptBlock` judges its signature then (a mutated block, whose sender is
punished, or the same block).

**Catching up.** Headers and blocks come as usual and are checked in full
(leader signatures included); an acknowledgement waits until the node's own
record has the commitment it names. The tickets of the newest commitments, not
acknowledged yet, are asked for with `fbgettickets`: at each new mainchain
block, a node asks its peers for those of the commitments on record after the
last acknowledged one that it has neither a ticket nor a block of.

## RPCs

| RPC | |
|---|---|
| `getposinfo` | The validators of the current epoch, slot and leader, anchors, the pot and its reserve, staking's totals, the producer |
| `getvalidators [epoch]` | The set of an epoch as the active chain fixes it (`known`, `seedknown`), the slots each one leads, and whether the active chain banned or jailed it since (`banned`, `jaileduntilepoch`) |
| `getvalidator <pubkey>` | One validator: stake, slots, next slot, offences known, its bond (owner, bonded, unbonds and when they are paid, jail, ban) |
| `signbondpop <owner> [pubkey]` | The proof of possession for a bond owned by `owner`, by a key of this node's producer |
| `getleaderschedule [epoch]` | The leader of each slot of an epoch (a banned one as `banned`: nobody leads the slot) |
| `getanchorinfo [count]` | The newest anchors, the anchored and final tips, the commitments the chain skipped, the pot, the tickets known |
| `getsidechainstate` | Adds `pegbalance`: the coins the chain holds against the escrow (below) |
| `submitevidence <hex>`, `listevidence` | Evidence (above) |
| `setstaking true\|false`, `getstakinginfo [since]` | This node as a validator: on or off (on at start), its keys and their bonds, what it made (`produced` since it started; `blocksmade`, the blocks of stake on the active chain its keys signed, and `blocksmadesince` a time), its next slot |
| `createbond <pubkey> <amount> [pop] [owner]` (wallet) | Bond coins of the wallet to a key. The owner is the bond's, else that of a bond of this wallet to the key waiting in the mempool (bonding twice before a block names one owner), else the one given, else a new address; with a pop for a first bond, the owner it was signed for must be given. Without a pop, for a key this node does not hold, the error names the owner to have `signbondpop` sign for on the validator's node |
| `createunbond <pubkey> [amount\|"all"]` (wallet) | Unbond some of the wallet's bond, or all of it (the default: all that is bonded when it is mined). One at a time: an unbond of the key in the mempool (named in the error) waits for a block first |
| `listbonds [all]` (wallet) | The wallet's bonds (or every validator's) |
| `listtransactions`, `listsinceblock`, `gettransaction` (wallet) | A coinbase output's `income`: `staked`, `anchor` or `reporter` (below) |

## Staking income

Not consensus: what wallets show. A coinbase output (`pos::CoinbaseRoles`,
through the chain interface's `getCoinbaseRoles`) is

- **staked**: the first output of a block of stake, the producer's share of
  its fees (paid to `-validatorpayout`, else a new address of the node's first
  wallet, else the first validator key's P2WPKH address);
- **anchor**: in a block that takes the peg actions (an acknowledgement, or a
  merged mined block), the pot and the share of the reserve paid to the
  anchor's payee (the ticket's, or the merged mined block's first output);
- **reporter**: the reward for a proof that a leader signed twice, paid at the
  peg step after the block that carried the proof to that block's first
  output's script.

The rest (a merged mined block's first output, deposits, refunds, unbonds) is
"mined", as before. A block's payouts come after its first output in this
order: what the mainchain gave rise to, the pot, unbonds, rewards, refunds. So
for an anchor block the node reads the blocks back to the anchor before it (that
one included) for the proofs they carried (a leader's proofs against itself earn
nothing), takes the last payouts to each reporter's script as its rewards, and
the last other payout to the payee before them as the pot. This is exact unless
a block's payouts passed the cap of 1000 (some wait for the next peg step) or a
refund (or an unbond) goes to the same script in the same block. The roles of a
block are cached (10000 blocks).

The wallet's transactions carry the roles (`interfaces::WalletTx::
txout_coinbase_role`); the GUI's transaction list labels such outputs "Staked",
"BMM reward" and "Slashing reward" instead of "Mined", and its type filter
has the three. The wallet RPCs give a coinbase output's `income`.

## The staking page

`fastblocks-qt` has a Staking page (Alt+6), in a scroll area whose boxes go one
under the other in a narrow window. Its data comes from the RPCs above, fetched
off the GUI thread every 3 s while it is shown (`NodeRpc::RunAsync`), and from
the wallet's transactions:

- the chain: slot and leader, BMM commitments (anchors);
- this node as a validator: staking on or off (buttons), its keys, their bonded,
  unbonding and eligible stake, whether they are in the set, jailed or banned,
  their share of the set's stake (the share of slots to expect), the next slot
  they lead and when, the blocks of stake they made today and on the active
  chain (`blocksmade`, `blocksmadesince` local midnight) and since the node
  started, and the slots they missed since it started;
- staking income of the wallet, today and in all: staked, BMM rewards,
  slashing rewards (the transaction list's records of these types);
- the wallet's coins by finality (soft, BMM-confirmed, final), and how far the
  chain is BMM-confirmed and final;
- the validator set of the epoch (key, stake, share, slots, status: this node,
  in the set, jailed, banned) and the newest BMM blocks;
- the wallet's bonds, with a key (the bond selected, or this node's first key,
  if none is entered) and an amount: Bond, Add to bond (to a bond the wallet
  has), Unbond (the amount, at most what is bonded) and Unbond all. Each asks
  before it is sent; what the page can tell is wrong (no amount, not a key, no
  bond to add to or unbond from, more than is bonded) it says before sending,
  and the node's error otherwise.

## The block explorer

The block explorer lists the newest blocks (up to 1000) with, after the height,
their kind: **PoS** (a block of stake, a blue disc), **BMM** (a block of stake
that records a BMM commitment, an acknowledgement: the disc with an amber
diamond) or **PoW** (a merged mined block, an amber diamond; the marks are drawn
with an outline, by shape as well as colour, so that they read on every theme).
Then the slot and producer of a block of stake ("this node" for one of this
node's keys, else the key shortened, in full in the tooltip), the mainchain
block whose BMM commitment a BMM block records or that committed to a merged
mined block, and its finality (soft, BMM-confirmed, final; as of the anchored
and final tips of `getanchorinfo`). A block's details start with a summary
(kind, slot and producer, the BMM commitment it records or makes, finality,
proofs) above the whole of `getblock`. (The GUI says "BMM" where the RPCs and
this document say anchor and acknowledgement: what ties the chain to the
mainchain, and where deposits and withdrawals happen; the RPC names are
unchanged.) Everything is fetched off the GUI thread
(`NodeRpc::RunAsync`, `CallAsync`); blocks already read are not read again. On
a chain without blocks of stake the FastBlocks columns are hidden.

## Fork choice

`nChainWork` is `anchors · 2^96 + (2^32 − 1 − last commitment height) · 2^64 +
height`: more anchors wins; with as many, the chain whose last anchor the
mainchain committed to first; then the longer. A BMM block's commitment height
is the record's: when a reorg of the mainchain moves it, the follower takes the
block back and checks it again, and its key (and that of every block on it) is
computed anew. An anchor always wins over any
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

## Peg balance

`getsidechainstate` gives `pegbalance` as of the tip (design §6.3): `utxos`
(spendable coins), `withdrawals` (coins burned by withdrawals waiting or in a
bundle: amounts and mainchain fees), `pot` (accrued less paid), `bonded` and
`unbonding` stake, the `reserve` of penalties, the `rewards` owed for proofs,
`payouts` (owed and not paid yet: deposits, refunds, the pot, unbonds and
rewards due), their `total`, and `mainheight`, the mainchain block the chain acted on up to.
Without fees left unclaimed (a producer may claim less than its share; what it
leaves is lost) and coins burned (`OP_RETURN` outputs), `total` is the escrow
the mainchain held for slot 5 after the block `mainheight`; with them, less.
Every FastBlocks functional test checks this on every node after every step
(`test_framework/fastblocks.py`, `assert_peg_balance`).

## Producer and anchorer

- `-validatorkeyfile=<file>`: 32-byte secret keys in hex, one per line. At
  each slot one of them leads, the node builds a PoS block on its best chain,
  with the acknowledgement of the first committed ticket it can carry, notes
  the slot in its slashing protection file (synced to disk before signing),
  signs and relays the block. It signs deterministically: BIP340 with the
  auxiliary randomness all zero, so that the nonce comes from the key and the
  message alone (BIP340's own derivation, as RFC 6979 does for ECDSA). A block
  signed twice gets the same signature: no peer holds a copy of a block with
  another one. (The auxiliary randomness guards the nonce against side
  channels on the signing machine; a validator key signs nothing else.) It
  never signs at or below a slot it signed before, and refuses to start if another node holds the key file's lock. It
  makes no block while it has a block of more work than its tip to connect; a
  header of more work alone does not stop it (a leader can sign a header that
  claims an acknowledgement and keep its block).
- Acknowledgements are noted (to find the blocks that rest on a commitment the
  mainchain drops) only from blocks signed by the leader of their slot. What is
  noted is a bounded cache (`MAX_NOTED_ACKS`, oldest out first): the blocks that
  matter are found from the chain (`BlocksCommittedBy`, given the mainchain
  height the commitment had): on the active chain, the block whose nNonce is
  that height, from the tip down to the first acknowledgement below it (not
  stopped by a BMM block: where the record has it now is not where it had it);
  and the blocks that failed against the record. Only a block whose nNonce is
  that height is read. Any other block is checked against the record when it is
  connected.
- The slots signed are kept in `<key file>.protection`, next to the key file
  (as is the lock, `<key file>.lock`): wiping the data directory to sync again
  keeps the key, and so must keep what it signed. The `validator_protection.dat`
  of older versions, in the data directory, is read too (the highest slot of
  each key) and its slots written to the new file at start.
- A slot whose block could not be made before the slashing protection noted it
  (nothing signed: the node still taking in a block that came as the slot
  started, the tip moving while the block was built, the seed of the epoch not
  on record yet) is tried again every half second for a third of the slot (at
  least a second). It counts once in `failed` if given up. A failure after the
  slot is noted is not retried: a second block would be a second signature.
- `-validatorpayout=<address>`: where the producer's share goes. Without it,
  a new address (label "validator payout") of the first wallet loaded when the
  node starts, so that the wallet sees the income and keeps it whatever becomes
  of the key file, which is hot by design. With no wallet, the first validator
  key's own P2WPKH address, with a warning in the log, which names the address.
  To see and spend what was paid there, import the key into a wallet: the
  descriptor `wpkh(<WIF>)` (`importdescriptors`, timestamp 0 to rescan), where
  the WIF is the key's 32 bytes in Base58Check with the chain's secret key
  prefix and the compressed flag `01`. `getstakinginfo` (`payout`) and the
  staking page ("Fees paid to") show where it goes.
- The anchorer: with `setbmm`, the node's merged miner; else, on a validator (a
  node with `-validatorkeyfile`), its own anchorer, on by default
  (`-validatoranchorbid=<amount>`, in coins of the mainchain: 0.00001 by default,
  1000 satoshis, about five times the relay fee of a request; 0 turns it off; off
  on regtest unless given), which pays the pot it wins to the producer's payout
  (`-validatorpayout`). It comes from the options, so a restart keeps it, with no
  `setbmm` to call again. While PoS blocks come, it makes a ticket for every
  mainchain block, as soon as it sees the block, and bids for its commitment
  (what the pot will pay, less 1 %; a validator at least its own bid, fees or no
  fees; `setbmm`'s `always` its amount). P is the block before its tip, or the
  tip itself once a ticket for the block before it could no longer be
  acknowledged in time (blocks of stake stopped for want of anchors); after the
  block P of a commitment of the last mainchain block still waiting for its
  acknowledgement (one ticket committed twice is one anchor: two mainchain
  blocks within a slot, the second gets the tip; none, if the tip is that P,
  until a block comes). It does not wait for that acknowledgement: the next
  mainchain block may come first, and would have no commitment of this chain
  (on the test network, with 30-second slots and mainchain blocks about a minute
  apart, a commitment in 62 % of mainchain blocks). Such a ticket's anchor builds
  no bundle (see Blocks): once the acknowledgement is in, if a bundle could
  start, the ticket is made again for a block after it, bidding 500 satoshis
  more, once. A request made as the mainchain moved on is kept (its ticket is
  for whatever mainchain block it follows). After `stall_seconds` without PoS
  blocks, it merged mines BMM blocks as the template does; a validator's own
  anchorer too, for its bid whatever the fees (a BMM block starts blocks of
  stake again).
- A leader acknowledges the first committed ticket that its block can carry in
  time (`bad-pos-unanchored`), and puts in the proofs it knows the chain has
  not punished. If the signed block does not pass validation, the slot is lost:
  a second block of the slot would be a second signature, which the node (it
  noted the first header) would take for proof against its own key; the
  acknowledgement and proofs it carried are left out of the producer's next
  blocks. It does not acknowledge a commitment
  whose acknowledging block failed when the record lost the commitment: the
  follower gives that block back once the record has it again (acknowledging
  it on a second branch only forked the chain).
- A peer that sends the header of a block that failed against the record, or of
  one on it, is not punished (template hook `PardonRecordFailures`, on for
  every sidechain of the template since 2026-10-12): its record may be ahead
  of this node's. A block that breaks a rule of FastBlocks whatever the record
  says (template hook `RecordIndependent`: the form of its sections, coinbase
  and proofs, its producer's share, where its acknowledged block is, its chain's
  time without anchors, the staking rules) did not fail against the record: it
  is not tried again when the record changes, and headers on it are punished.
  Not a failure that reads a leader (`bad-pos-sig`: the seed is the record's),
  a set, a commitment or the mainchain's times.
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

- Phase 4 came before phase 3 (bonds and slashing came after it).
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

## Deviations from the design (phase 3)

- The set of an anchor is the state's when the anchor's block takes the peg
  actions (the chain up to its parent), not the state at P: an acknowledgement
  is at most a few blocks after P, and the set is keyed by the anchor's block
  on its own branch, so every node of a branch has the same.
- Regtest's seed lookahead is an hour, not 0 (`-posseedlookahead` sets it):
  a regtest mainchain's median time lags or leads the clock by minutes as the
  tests mine it, and blocks of stake would wait for mainchain blocks. The unit
  tests take it to 0.
- A header whose epoch's set the node does not know (the anchor that made it is
  on a branch the node never connected) is taken, and its leader checked when
  its block is connected, rather than waiting: a waiting header could deadlock
  a node that needs the branch to learn the set (a partition with anchors on
  both sides). Since audit 6, only on a branch that leaves the active chain at
  or above its final block and above the sets the state pruned (see "Headers
  the node cannot check yet").
- The window for proofs counts anchors since the anchor that made the set of
  the offence's epoch. (BMM blocks carry proofs too, since 2026-10-12: each in
  a coinbase output of its own, rather than a commitment to their root.)
- No correlated penalty above 1/3 of the stake, no forced unbond after three
  jails (design 5.2): not in the decisions of section 13.
- Liveness charges at most an epoch of empty slots per block of stake; a
  BMM block's epoch, for counting liveness and pruning, is taken two hours
  before its time (it may be that far ahead of the clock); the set it makes,
  and the jails it gives, are as of the epoch of the median time of the
  mainchain block that committed to it (see Jailing).
- A validator is slashed once: proofs of other offences of a banned validator
  are not taken.

## Tests, fuzzing and the audit (phase 5)

Unit tests: `src/test/fastblocks_tests.cpp` (every reject reason above is
asserted there). Functional tests, each checking `pegbalance` on every node
after every step:

| Test | What |
|---|---|
| `feature_fastblocks.py` | Validators, anchors, the fee split and pot, a withdrawal, slashing protection, BMM blocks with transactions when stalled |
| `feature_fastblocks_rpc.py` | RPCs, the wallet's finality, tickets and evidence over P2P, a late observer |
| `feature_fastblocks_partition.py` | A leader that signs twice; a partition with an anchor on each side (the earlier one wins); an anchor of a branch the validators left; IBD |
| `feature_fastblocks_signature.py` | A block with the hash of a known header and another signature (another key's, or none), relayed whole, as a compact block and submitted, is turned away; the block its leader signed is taken after, and stays through `-reindex`; the producer's signatures are deterministic; another valid signature of the leader is the same block (taken, the node's own kept, no peer disconnected; two nodes holding the two stay in sync) |
| `feature_fastblocks_stall.py` | A mainchain reorg that drops and gives back an anchor; blocks of stake stalled (BMM blocks carry the transactions, PoS starts again); anchors stalled (PoS halts after `max_unanchored_slots`, starts again with a new ticket); IBD |
| `feature_fastblocks_staking.py` | No validators (merged mined only); bonds (one with a proof of possession made by the test); blocks of stake from the epoch after next; a key that runs nowhere is jailed, its coins kept; a stranger's top-up of a satoshi refused, the owner's taken; an unbond of all paid unbonding_anchors anchors later |
| `feature_fastblocks_p2p.py` | Peers of the test framework that misbehave: forged signatures sent first to a node catching up, then the honest nodes (it reaches their tip; the honest blocks are not fetched again and again meanwhile); 5000 unsigned siblings of its newest header, and a message padded with more (the peer's budget taken, no more); about 70 unsigned acknowledging headers from deep in the chain, on a validator that knows the sets (`bad-pos-sig`) and on one restarted whose state pruned them (`bad-pos-fork-too-deep`: no new tip, no reorg); 5000 siblings of a deep block; tickets 8 a peer per mainchain block; `fbgettickets` of 17 entries (misbehaving); one proof sent 200 times (kept once, the peer kept) |
| `feature_fastblocks_fork.py` | A fork whose overtaking block a node has the header of with a forged signature, and cannot check before it connects the fork: it syncs, the leader's signature replacing the forged one |
| `feature_fastblocks_idle.py` | Validators only, no fees, no `setbmm`, 30-second slots on mock time: an hour and more of blocks of stake and anchors; mainchain blocks in pairs, a commitment in at least 90 % of them, each acknowledged; staking off everywhere, the validators merge mine; restarted, they bid again |
| `feature_fastblocks_restart.py` | A validator restarted refuses a BMM block 30 minutes ahead as the others do; every validator restarted while a committed ticket waits asks for it and acknowledges it; a validator whose data directory is wiped does not sign a slot it signed again; one killed between signing and relaying (`-posfaultaftersign`) does not sign that slot again |
| `feature_fastblocks_soak.py` (extended) | 30-second slots (`-posslotseconds=30`), `--duration` minutes of rounds: restarts and kills, partitions, mainchain reorgs, bonds, unbonds, payments, withdrawals; after each, one tip, `pegbalance`, no node down, few failed slots |
| `feature_fastblocks_slashing.py` | Bonded validators; a leader signs twice; the proof in a block: 10 % gone, banned, the rest unbonded; the banned validator leads no more from the next block; a proof of another of its offences takes nothing; a leader that carries the proof against itself gets no reward; a BMM block carries a proof (all validators stopped), its payee rewarded; the reward paid at the next peg step to the block that carried it; a forged proof refused; the offender out of the sets |
| `feature_fastblocks_window.py` | Observer with small budgets of headers it cannot check (`-posuncheckedheaders`, `-posuncheckedbytes`): a partition branch several budgets long, synced in windows from one outbound peer while inbound attackers hold their whole budgets; a branch whose part before it leads exceeds the budgets (both sides anchored): not followed; the operator's `invalidateblock` of its side's first block after the fork, then `reconsiderblock`, takes it |

Fuzz targets (`src/test/fuzz/fastblocks.cpp`): `pos_header`, `pos_ticket`,
`pos_evidence`, `pos_state` (a whole chain from fuzzed choices, with bonds,
unbonds and leaders that sign twice: the state agrees with the chain key, the
totals of staking with its validators, the coins are the deposits, a tip taken
back and connected again gives the same state), `pos_staking` (the staking
rules alone with fuzzed parameters: every coin bonded is accounted for, the
journal takes any step back), `pos_cold_hooks` (hooks rebuilt mid-chain, as on
a restart, against those that saw the chain from its start: the same state
transition for every block to come, the same time limit of blocks, final
height, chain key, and leaders and header outcomes where both can tell the
leader).

What the audit of this round changed (each with a test):

- `max_unanchored_slots` 120 on 30-second networks: an hour, as decided (the
  design's table had 1200 and 600, ten and five hours).
- Producers are not stopped by a header of more work whose block never comes.
- Tickets from peers: 8 a peer per mainchain block of those not committed to.
- Acknowledgements are noted only from blocks their leader signed; evidence
  that is no proof is misbehaving.
- Blocks at most 10 minutes ahead of the clock while PoS is on (template hook
  `MaxFutureBlockTime`).
- A BMM block's chain key follows its commitment when a mainchain reorg moves it
  (template: `ReconsiderRecordFailure` recomputes `nChainWork`).
- The best header may be lower than the tip (template:
  `GuessVerificationProgress` no longer assumes otherwise; a debug build
  aborted).
- Finality counts anchors net of skipped commitments (a hidden ticket could
  take back an "anchored" block with one bid and one slot).
- After an anchor stall, a ticket that came too late is not waited for nor
  tried; a new one names the tip if needed.
- `nPowTargetSpacing` is the slot (Bitcoin's heuristics count blocks by it).
- A header of a slot to come waits only if its leader signed it.
- A block for a header the node has must carry its signature
  (`bad-blk-extension`): a node could otherwise store, and serve, a block
  of stake with another or no signature (template: `AcceptBlock`, compact
  blocks; a block stored so before is read with its header's signature).
- The bundle is fixed by P: its lock time and the height a failure counts
  from are P's (template hook `PegStep::peg_height`), not those of the
  acknowledging block.

## The audit of staking

Tests, fuzzing and coverage of phase 3, and an adversarial review of it (stake
grinding, nothing at stake, unbonding against the window for proofs, slashing
nobody can avoid or that others can cause, validator sets across reorgs and
anchors, jailing, the reporter's reward, the peg balance). What it changed,
each with a regression test:

- The producer never signs a second block for a slot. A block found invalid
  once signed (with an acknowledgement or proofs) was made again without them
  and signed again; the node, which had noted the first header, took the two
  for proof against its own key, relayed it, and put it in its next block:
  an honest validator slashed. Now the slot is lost, and the ticket and the
  proofs are left out of the producer's next blocks (`one_signature_per_slot`).
- Leaders are not charged for slots at which no block could be made: those
  `max_unanchored_slots` or more after the first uncovered block of stake, and
  those up to a BMM block the block follows. A stop for want of anchors could
  jail a whole small set (`no_jail_for_slots_nobody_could_fill`).
- The rules of a block read the sets of anchors from its chain's state only,
  not from the sets the node keeps in memory (a set the state pruned, or one an
  earlier record made, could decide validity on one node and not another). A
  mainchain reorg that moves the seed of an epoch connects its blocks of stake
  again from its start, not only those whose own leader changed: the liveness
  they charged (and the jailing, and the sets after) were the old seed's.
  `CheckSeeds` no longer holds the set mutex while it checks blocks it took
  back (the follower deadlocked with `cs_main` held once a block whose set an
  anchor made was given back) (`seed_moved_state_connected_again`,
  `leader_checked_when_connected`).
- An anchor's slot is not before the median time of the mainchain block that
  committed to it: a merged miner that makes every block after a stall could
  otherwise set two BMM blocks' times back and fix, with bonds, the set of an
  epoch whose seed is known (`anchor_slot_from_the_commitment`).
- `createbond` and `listbonds` ask the wallet about its scripts with its lock
  held (a data race; a debug build aborted, and the Qt staking test hung).
- A unit test of a penalty larger than the bond (taken from the latest
  unbonds); `pos_state` fuzzes bonds, unbonds and leaders that sign twice;
  `pos_staking` fuzzes the staking rules alone.

Found and left as they are (decisions):

- With `liveness_min_assigned` 10 per epoch, a validator is never jailed once
  it leads fewer than 10 slots an epoch: on the main network (120 slots) with
  13 or more equal validators, on the test networks (20 slots) unless it holds
  half the stake.

## The decisions of 2026-10-12

Each with a regression test (unit tests in `fastblocks_tests.cpp`, and the
functional tests named).

- Deterministic leader signatures (above): a leader could sign one block twice
  with different random nonces, and a node that had the header with one
  disconnected the peers that sent the block with the other
  (`bad-blk-extension`). Now the producer signs each block one way, and
  another valid signature of the leader over a block is the same block: the
  node keeps its own, punishes nobody, and does not split
  (`deterministic_signatures`, `feature_fastblocks_signature.py`).
- Slashed once: 10 % of a validator's stake, once. A banned validator's other
  offences take nothing more (`bad-pos-evidence-banned`, a block carries one
  proof per validator), and it leads no more from the block after the one that
  banned it (`bad-pos-leader-banned`): its slots stay empty, rather than wait
  two epochs for the sets to leave it out. The ban is in the state below each
  block, so every node agrees on it (`slashed_once_and_out_of_the_schedule`,
  `feature_fastblocks_slashing.py`).
- No reward for reporting oneself: a proof carried by a block of stake whose
  leader is the offender pays no reporter; the penalty applies all the same,
  and the reward's share goes to the reserve (`self_reported`,
  `feature_fastblocks_slashing.py`).
- Proofs in BMM blocks too: a merged mined block's coinbase may carry proofs,
  under the same rules, the reward to its payee, so that validators who stop
  making blocks of stake cannot keep a proof off the chain (template hook
  `MergeMinedOutputs`: the node's merged miner puts in the proofs it knows)
  (`evidence_in_bmm_blocks`, `feature_fastblocks_slashing.py`).
- `PardonRecordFailures` is on in the template for every sidechain (a node a
  little behind on the record no longer disconnects the peers that are not);
  the template's unit and functional tests pass with it
  (`sidechain_hooks_tests/record_failures_pardoned`).
- `feature_sidechain_record.py` failed about one run in three: it mined a
  block on the parent of a block the operator had invalidated, with the same
  coinbase, no transaction and, within one second, the same time: the very
  block, which the node rightly refused (`duplicate-invalid`). A test timing
  matter, not a bug: each block it mines now pays a new address.
- Bonds cannot be griefed: only the owner adds to a bond (it signs each
  later bond, `bad-pos-bond-owner-sig`), and an unbond of all (amount 0,
  `createunbond <key>` or `all`) takes all that is bonded when it is applied
  (`bond_griefing`, `bonds`, `unbonds`, `feature_fastblocks_staking.py`).
  Jailing is unchanged.

## Audit 6 (2026-10-13): consensus, network, producer and wallet

Each finding was checked first; each fix has a unit test (`fastblocks_tests.cpp`)
that failed before it. Consensus changes, with no activation height: the test
network starts again from genesis.

- A header whose leader the node could not tell yet (the set of its epoch made
  by an anchor it has not connected: every node that catches up) was taken
  unsigned, and counted as an anchor in the order of chains for any commitment
  on record. A branch of such headers from deep in the chain outranked it for
  free (the best header, block download, reorgs back to the fork; the block
  index grown without bound). Now an acknowledgement names a commitment after
  the last anchor below it (`bad-pos-ack-order`, a header rule), and a header
  the node cannot check is refused on a branch that leaves the active chain
  below its final block, or whose set the state pruned (`bad-pos-fork-too-deep`,
  not punished): see "Headers the node cannot check yet" (`forged_ack_headers`,
  `unsigned_ack_header_unknown_set`). Design: the node cannot check these
  headers (it would need the state of their branch), nor refuse them (it would
  never catch up), nor count them anew once checked (the order of chains must be
  one function of the headers on every node); so their weight is bounded.
- A peer could send the honest headers with forged signatures (the block hash
  does not cover them) to a node catching up; the honest block was turned away
  for good after, and with the forged block too, its hash failed for good. Now
  the valid signature replaces the forged one (template: `REPLACE`,
  `Chainstate::ReplaceBlockExtension`), and the failure is taken back
  (`forged_signature_replaced`).
- Every failure of FastBlocks' rules counted as a failure against the record:
  retried at each start and recommit, its peers pardoned. Now the template hook
  `RecordIndependent` names those that are not (`record_independent_failures`).
- A header of a slot to come whose leader the node could not tell waited (the
  bounded set of waiting headers; anyone can make them). Now only a header its
  leader signed waits (`pos-time-future-unchecked`;
  `future_unsigned_header_not_waiting`).
- A proof sent again cost two signature checks under `cs_main`: now the pool is
  looked at first (`evidence_replay_checked_first`).
- The headers noted to find proofs were keyed by slot: another leader's header
  of the slot (another branch's) hid a leader's two. Now by slot and signer
  (`note_signed_per_leader`).
- A header signed on another network (the slots of the networks line up) made a
  valid proof here: an operator running one key on two networks was slashed.
  The block signature commits to the genesis hash (`evidence_other_network`).
  The other signed messages: bonds' proofs of possession and owners' messages,
  and unbonds, did already; a ticket names a block of its chain, and the
  mainchain commits to it in the chain's slot.
- An unbond the owner signed and that was not mined could be mined by anyone
  later, after the owner added to the bond, taking that too. A top-up moves the
  unbond sequence on (`unbond_replay_after_topup`).
- A stranger could copy the public proof of possession of a pending first bond
  into a bond of its own, mined first. The owner signs first bonds too
  (`first_bond_signed`; `createbond` signs every bond).
- The 10 minute limit on block times (while the network has validators) was
  known only once a node connected an anchor with an active set: a restarted
  node took blocks two hours ahead, and the flag never went back on a reorg. Now
  it is the state of the tip's (`staking_seen_from_the_state`).
- Fuzzing: `pos_header` checks that headers nobody could check count one anchor
  at most per new commitment height, and that signatures do not check under
  another genesis; `pos_state` sends headers from peers (signed or not, sets
  forgotten as on a restart) and proofs with a header from another network or
  another leader, and checks that what is taken keeps the rules.
- Checked: `WaitingReady` gives a waiting header and its waiting parent
  different keys (the parent's height plus one), so the template's lost-header
  case does not arise.

### Part 2: the producer and the wallet

- The producer tried each slot once: a failure before the slashing protection
  noted the slot (the node still taking in a block that came as the slot
  started, the tip moving while the block was built, the seed not on record
  yet) lost the slot, though nothing was signed. It now tries again for a third
  of the slot (`producer_retries_before_signing`).
- The slashing protection was in the data directory, the key elsewhere: wiping
  the data directory to sync again kept the key and lost the slots it signed.
  It is now next to the key file, the old file read too
  (`protection_next_to_the_key_file`).
- A bond or an unbond was checked against the tip once, as it entered the
  mempool. A block that moved the state under it (a slash, a second unbond of
  the same sequence mined, a first bond naming another owner mined) left it
  there for two weeks, its inputs held, and `createunbond` refused (an unbond
  waited) and `abandontransaction` too (it was in the mempool). Now the mempool
  checks such transactions again after every change of its tip, and drops those
  the next block could no longer take (template hook `HasTxRules`;
  `mempool_drops_stale_staking_txs`, and the slashing functional test). The
  wallet's one-unbond-at-a-time check counts only an unbond in the mempool, and
  names it.
- `createbond` twice before a block (the button clicked twice) made a second
  owner, whose bond could never be mined after the first. It now names the
  owner of a pending bond of the wallet to the same key (staking functional
  test). With a pop and no owner, a first bond is refused at once (a new
  address could not match the pop); without a pop, for a key the node does not
  hold, the error says to run `signbondpop <key> <owner>` and pass that owner.
- The wallet's finality was computed against the node's tip: during a reorg a
  payment the reorg took out showed as anchored or final by the other branch's
  anchors. Now against the wallet's last block, once per call
  (`wallet_finality_on_its_own_chain`, in `fastblocks_tests.cpp` as it needs
  the FastBlocks chain).
- Without `-validatorpayout`, the producer's share went to the hot key's own
  address, which no wallet saw. Now to a new address of the node's wallet,
  if one is loaded (`feature_fastblocks.py`); else the key's, with a warning.
- Wrong: "immature coinbase outputs are in the finality buckets for 100
  blocks". FastBlocks' coinbase maturity is 0 on every network: a coinbase
  output is spendable in the next block, and none is ever immature once
  confirmed. Nothing changed.
- Tests: `-posfinalanchors` (regtest) lets `feature_fastblocks_rpc.py` show the
  anchored state on a node that counts three anchors to final.

### Part 3: the tests the audit found missing

New test classes: `feature_fastblocks_p2p.py` (peers of the test framework that
misbehave), `feature_fastblocks_restart.py` (restarts, a wiped data directory, a
validator killed between signing and relaying), the fuzz target
`pos_cold_hooks` (hooks rebuilt mid-chain against warm ones), and
`feature_fastblocks_soak.py`, extended (30-second slots, random restarts,
partitions, mainchain reorgs and transactions). Regtest options for them:
`-posslotseconds=<n>` (slots of n seconds, stalls of 30 slots) and
`-posfaultaftersign` (the node stops at once after its producer signs a block).
What they found, each fixed with the test that failed before:

- A node catching up took headers whose signer it could not tell yet in any
  number: thousands of unsigned siblings of its newest header, for free, each
  one an entry of the block index. Such a header is now taken only if it leads
  past the best header, or comes in a message whose headers do
  (`pos-unchecked-low-work`; `unchecked_siblings_not_taken`, and the p2p test).
- A block with the leader's signature, for a header the node had with a forged
  one, could not be checked before the chain below it was connected
  (`blk-extension-unknown`), and was asked for again at once: as fast as the
  honest peers sent it, as long as a peer held the blocks below in flight
  (about 20000 times a second in the p2p test). Now not before its parent is
  connected (template, `net_processing`).
- A node catching up whose last header from each peer failed through no fault
  of the peer (a slot a few seconds ahead of its clock, whose signer it could
  not tell yet) did not note that the peer had the headers before it, and
  fetched no block at all: it never came to tell (the restart test: a validator
  whose data directory was wiped, its clock 3 seconds behind, stayed at
  genesis). Now the headers taken before are the peer's (template).
- A block that failed against the record when connected (an acknowledgement
  whose commitment a mainchain reorg moved) was BLOCK_CONSENSUS: the peer that
  had relayed it was disconnected, though record failures are pardoned for
  headers. In the soak test, after a mainchain reorg every node cut off the
  validator that had relayed such a block, which then made blocks alone. Now
  not punished; a rule broken whatever the record says still is (template;
  `block_failing_against_the_record_pardoned`).
- Checked, no change: after a restart the time limit of blocks is the tip
  state's (the restart test fails with the audit's fix taken out); every
  validator restarted with a committed ticket unacknowledged gets it back from
  a peer and acknowledges it; a wiped data directory or a kill between the
  signature and the relay never signs a slot twice.
- `pos_cold_hooks` holds for the whole state transition of blocks, the time
  limit, final heights and leaders; it finds the restart bug of the time limit
  when it is put back. Finality is what a node knows: a node without the
  tickets of skipped commitments counts them against every fork (the target
  gives the rebuilt hooks the tickets, as peers do).
- `producer.cpp` did not build with clang (an ambiguous path division).
- The soak test ran 25 minutes after these fixes (41 rounds: 6 restarts or
  kills, 5 partitions, 2 mainchain reorgs, 18 bonds and unbonds, 10 payments
  and withdrawals): one tip after every round, `pegbalance` on every node, no
  evidence, no failed slot. `pos_cold_hooks` ran 10 minutes without a finding.

### Review of audit 6 (2026-10-13)

`~/skydoge/audit/2026-10-13-audit-6-review-findings.md`, each finding checked
first; each fix with a test that failed before it. (The fixes of findings 1, 2,
6, 7 and 8 below were replaced, after a second review found holes in them, by one
design: "Headers and blocks the node cannot check yet".)

- A block a node held back (`blk-extension-unknown`: another signature than its
  header's, which the node cannot check before it connects the chain below) was
  asked for again only once its parent was connected. When that block is the one
  that makes a fork overtake, the parent is never connected: the fork's download
  stalled for good, from every peer, after one peer sent the header with a forged
  signature. Now no block is held back: it is taken with the header's signature,
  the other one kept and tried when the block is connected (see "Headers the node
  cannot check yet"; `block_extension_unknown_taken`, `feature_fastblocks_fork.py`).
  A block stored with a forged signature (sent whole) that fails when connected is
  asked for again from the peers that have it (`feature_fastblocks_p2p.py`, which
  then stalled now and then).
- `pos-unchecked-low-work` compared what unchecked headers claim with the best
  header, both of which an attacker sets: a padded message passed it, and an
  inflated best header turned away an honest node's headers. Now a count of such
  headers per peer, given back as they are checked (`pos-unchecked-budget`;
  `unchecked_siblings_not_taken`, `unchecked_headers_checked_give_room`, the p2p
  test).
- Checked: no `TMP_` or other in-memory consensus flag is in the code (`grep -rn
  TMP_ src/` finds only the SHA-256 assembly's macros); the 10-minute limit on
  block times (`MaxFutureBlockTime`) is read from the state of the tip, cached by
  the tip's hash only.
- An idle chain halted: tickets were bid only for blocks with fees (`setbmm`
  without `always`), and `setbmm` did not outlive a restart; without anchors,
  blocks of stake stop after `max_unanchored_slots`, and BMM blocks needed fees
  too. Now a validator's own anchorer bids for every mainchain block whatever the
  fees, on by default (`-validatoranchorbid`), and merge mines when blocks of
  stake stall (`feature_fastblocks_idle.py`, which fails with it off).
- The anchorer waited for its committed ticket's acknowledgement before it made
  the next: every mainchain block that came before it had no commitment of the
  chain (62 % on the test network). It bids at once now; consensus lets a ticket
  anchor a block below the last acknowledgement (at or after the block the last
  anchor anchored), whose anchor then builds no bundle (`acknowledgements`, unit
  test; `feature_fastblocks_idle.py`: a commitment in at least 90 % of mainchain
  blocks that come in pairs, 50 % with the wait put back). Two more found on the
  way: a ticket made again for the same block and payee was the same ticket,
  committed twice, one anchor; and a request made as the mainchain moved on was
  in its mempool, but its ticket was dropped, so its commitment was one nobody
  could acknowledge.
- The GUI calls a block of stake that records a BMM commitment a "BMM" block,
  the states soft, BMM-confirmed and final, and the anchor's pot "BMM reward(s)";
  the RPCs keep their names.

### Closing the known gaps (2026-10-14)

- **Pointers into the block index.** Unchecked headers leave the block index,
  freed a batch later; no path relies on that delay any more (template; doc/sidechain.md,
  "Pointers into the block index"). Found on the way: `InvalidateBlock` kept a map of
  entries across releases of `cs_main`, and one that left could become the best
  header (a dangling pointer); `getblock` and the wallet's `findBlock` read the
  block of an unchecked header without the lock (its file deleted meanwhile);
  `getblocktxn` asserted a read that a deleted file fails; the RPCs, REST calls
  and the producer that read a block they looked up without the lock. Each path
  now holds the lock, looks the entry up again, or never sees unchecked headers;
  `unchecked_erased_while_used` has a peer leave exactly where each uses an entry
  (it fails with any of the getblock, findBlock or InvalidateBlock fixes taken
  out).
- **Item 8a of the review** (the list of 64 failed blocks asked for again, which
  an attacker could fill): the list is gone since the one design; nothing is
  kept for a failure but one pending request a peer, now merged with a pending
  one rather than replaced. `extension_failure_asked_again_whatever_fails`: 300
  failures of an attacker's own around an honest block's, which is still asked
  for and connects.
- **Tests on a busy machine.** `feature_fastblocks_partition.py` ran on the
  clock: under load, more than `max_unanchored_slots` passed between two anchors
  and blocks of stake stopped (it failed five times in five with four tests at
  once). It runs on mock time now, moved a slot at a time once the slot's
  leaders made their blocks and their peers have them (`SlotClock`, test
  framework: a block that reached the next leader late was left out, and its
  maker jailed for the slot). `feature_fastblocks_idle.py` waits for each bid
  that is due rather than 10 seconds; `feature_fastblocks_restart.py` read the
  slot a validator signed last while its block was still on its way (it stops
  the validators and checks that block is on every chain first). Every wait is
  on a condition, with generous bounds; each of these, and
  `feature_fastblocks_window.py`, passed five times in five with four tests at
  once.
- **A long partition branch** syncs in windows once the part the node holds
  leads; the limit before it leads, why it stays, and the operator's way past
  it: see "The limit of a long branch" above (`feature_fastblocks_window.py`;
  regtest options `-posuncheckedheaders` and `-posuncheckedbytes`).

### Review 6 (2026-10-15)

- **A leader signed headers of its slot without bound** (the one attack that
  worked): each was checked and taken, a permanent block index entry on every
  node, at the cost of a signature. Fixed at the root, the cost of a header: at
  most two headers of a slot are kept for good; a further one from a peer is
  spare, taken only with a header on top of it and then unchecked (see "Headers of
  a slot"; template "Spare headers"). `attack6_leader_signs_headers_without_bound`
  (2000 headers of one slot: one taken, the proof; the rest `header-spare`),
  `slot_spare_header_taken_on_a_branch`, and the template's
  `spare_headers_only_on_a_branch` (taken with a header on top, gone with its
  peer, kept for good once connected).

### Review 7 (2026-10-15)

Three attacks worked; each fixed at its root (tests in `fastblocks_tests.cpp`):

- **A spare header sent again** cost three signature checks a copy, unblamed
  (audit 6 review, item 15). A header found spare has no signature checked: it
  can change nothing (see "Headers of a slot").
  `attack7_spare_header_replayed_costs_signatures`.
- **A header of an early slot** made the node walk the whole chain under
  `cs_main` before `time-too-old` turned it away (`HeaderCheckable` read its
  leader first): the time is looked at first.
  `attack7_early_slot_header_walks_the_chain`.
- **A mainchain reorg reaching a seed more than three epochs back** split
  running nodes from fresh ones (item 16): the seeds of every epoch are kept,
  checked newest first down to the first unmoved, and a block that comes between
  a reorg and the check waits (`pos-seed-moved`; see Seed).
  `attack7_seed_moved_beyond_the_kept_epochs`, `seed_moved_then_a_block_before_the_check`.
- **Noted acknowledgements pushed out** (the security review's open lead). The
  blocks resting on a commitment were found only among the acknowledgements
  noted in memory (20000, oldest out first), noted from any block that reads and
  that its slot's leader signed, before its header is checked. One validator
  key (a current or a former one) signs 20001 blocks of one of its slots, each
  acknowledging nothing on record: when the mainchain then drops a ticket's
  commitment, the flooded node keeps the block that acknowledged it (an anchor
  that no longer exists) while the others take it back, and does not take back
  a block that failed when the commitment returns. Now `BlocksCommittedBy` is
  given the mainchain height the commitment had (the follower knows the height
  of each block it drops) and finds the blocks of the active chain and those
  that failed against the record from the chain; the noted ones are a cache.
  `attack8_acks_pushed_out_still_taken_back`.

### Review 8 (2026-10-15)

The security review of the attack8 fix found two issues in `BlocksCommittedBy`
(`src/pos/hooks.cpp`), both real; and the same split at startup:

- **A split.** The walk from the tip stopped at the first BMM block the record
  committed below the height. The record is the one after the mainchain reorg:
  a reorg that drops a ticket's commitment and commits again, lower, to a BMM
  block above its acknowledgement stopped the walk above it, so a node that had
  not noted the acknowledgement (restarted, or flooded: attack8) kept it on an
  anchor the mainchain dropped while the others took it back. Acknowledgements
  are in strict mainchain order on nNonce (the state's `bad-pos-ack-order`,
  connect time, and the header's), which the block hash covers: the walk stops
  only at an acknowledgement below the height. It visits the blocks above the
  last one: each BMM block among them is a commitment of its own, with at most
  `max_unanchored_slots` blocks of stake after each anchor. A commitment to a
  BMM block of ours needs no walk (acknowledgements name tickets).
  `review8_ack_below_a_bmm_block_committed_lower_taken_back`.
- **A denial of service.** Every call read from disk, under `cs_main`, each
  acknowledgement of the active chain at or above the height (a mainchain reorg
  of N blocks: N^2/2 reads) and each block failed against the record, which are
  never forgotten and cost no commitment (a leader makes two of each slot it
  leads, acknowledging at the height of any commitment on record a ticket nobody
  committed). It is called for each commitment the mainchain makes or drops, and
  for each ticket a producer may acknowledge, each slot (`NextAck`); with the
  noted acknowledgements pushed out (attack8), each one a read. Only a block
  whose nNonce is the height can rest on the commitment (`bad-pos-ack-height`):
  only those are read, the others are passed over from the block index. (The
  failures are still all looked up in memory; `NextAck` counts only one at the
  height, whatever this node noted.)
  `review8_blocks_committed_by_reads_only_the_candidates`.
- **At startup** (`Follower::CheckActiveChain`, for a node that stopped between
  learning of a change of the mainchain and acting on it) only blocks without a
  commitment at the top went: in the reorg above, the block committed lower was
  checked again as moved and connected again on its parent, which has no
  commitment, where a running node took back both. The lowest block without a
  commitment within `MOVED_CHECK_DEPTH` of the record's tip goes, with all
  above it. `feature_sidechain_record.py` ("the block on it committed to again
  below it").

## Deviations from the design (phase 5)

- Leaders do not move to an anchored branch they left (§4.2): such an anchor
  is acknowledged only by leaders on that branch, and is otherwise void.
  Finality counts it against the blocks it could take back.
- (Phase 3 is built since: see Staking.)
