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
| Slot | 30 s | 30 s | 1 s |
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
height (design §4.3.4).

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
sets the state keeps (`bad-pos-fork-too-deep`). So a branch of headers nobody
checked counts at most as many anchors as the commitments it names, after one it
paid for; when its first block is connected, the signature fails and the node is
back on its chain. A header taken with a signature that turns out invalid (the
block hash does not cover it: a peer can send the honest header with another)
is replaced by the valid one when it comes (template: `CheckOtherExtension`
says `REPLACE`; the header or block with it replaces the extension, the stored
block is written again with it, and a failure of the block with the forged one,
`bad-pos-sig`, is taken back).

A header that may come later (`pos-time-future`, `pos-ack-unknown`,
`pos-seed-unknown`, and `bmm-unknown` for a BMM block) waits in the template's bounded set of waiting
headers, with the headers a peer sent after it; each is taken in once it and
every waiting header below it can be, in the order of their heights.

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
| For a header the node has, the block carries that header's signature and sections hash: the block hash does not cover the signature (template, `AcceptBlock`; a mutated block, the header stays valid; a compact block with another one is not reconstructed). Another valid signature of the slot's leader is the same block (template hook `CheckOtherExtension`): taken with the node's own signature, which it keeps, and its peer not punished; if the node's own is not valid (taken before it could tell the leader), the valid one replaces it (see above); one the node cannot check yet (it cannot tell the leader) is not taken, and not punished either (`blk-extension-unknown`) | `bad-blk-extension` |

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
| A PoS block is signed by the leader of its slot, as the chain below it and the record say when it is connected (its header may have come before the node knew the epoch's set; the record may have moved the seed since) | `bad-pos-sig`, `bad-pos-inactive`, `pos-seed-unknown`, `bad-pos-set-unknown` (a set the state no longer has) |
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
so the jailing, and the sets made after) are the new seed's. The rules of a
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
signature is checked, and dropped.

## P2P

| Message | Content | Rule |
|---|---|---|
| `fbticket` | A ticket | Kept and relayed if the mainchain committed to it, or if it names a block the node knows: of those, 8 a peer per mainchain block at most (anyone can make them by the thousand) |
| `fbgettickets` | Up to 16 h* | The node sends the tickets it has; more than 16 is misbehaving |
| `fbevidence` | Two headers | Kept and relayed if it is a proof (above); a peer that sends one that is not misbehaves (not one whose leader the node cannot tell yet) |

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
"Anchor reward" and "Slashing reward" instead of "Mined", and its type filter
has the three. The wallet RPCs give a coinbase output's `income`.

## The staking page

`fastblocks-qt` has a Staking page (Alt+6), in a scroll area whose boxes go one
under the other in a narrow window. Its data comes from the RPCs above, fetched
off the GUI thread every 3 s while it is shown (`NodeRpc::RunAsync`), and from
the wallet's transactions:

- the chain: slot and leader, anchors;
- this node as a validator: staking on or off (buttons), its keys, their bonded,
  unbonding and eligible stake, whether they are in the set, jailed or banned,
  their share of the set's stake (the share of slots to expect), the next slot
  they lead and when, the blocks of stake they made today and on the active
  chain (`blocksmade`, `blocksmadesince` local midnight) and since the node
  started, and the slots they missed since it started;
- staking income of the wallet, today and in all: staked, anchor rewards,
  slashing rewards (the transaction list's records of these types);
- the wallet's coins by finality (soft, anchored, final), and how far the chain
  is anchored and final;
- the validator set of the epoch (key, stake, share, slots, status: this node,
  in the set, jailed, banned) and the newest anchors;
- the wallet's bonds, with a key (the bond selected, or this node's first key,
  if none is entered) and an amount: Bond, Add to bond (to a bond the wallet
  has), Unbond (the amount, at most what is bonded) and Unbond all. Each asks
  before it is sent; what the page can tell is wrong (no amount, not a key, no
  bond to add to or unbond from, more than is bonded) it says before sending,
  and the node's error otherwise.

## The block explorer

The block explorer lists the newest blocks (up to 1000) with, after the height,
their kind: **PoS** (a block of stake, a blue disc) or **PoW** (a merged mined
block, an amber diamond; the marks are drawn with an outline, by shape as well
as colour, so that they read on every theme). Then the slot and producer of a
block of stake ("this node" for one of this node's keys, else the key
shortened, in full in the tooltip), the mainchain block that committed to a
merged mined block, and its finality (soft, anchored, final; as of the anchored
and final tips of `getanchorinfo`). A block's details start with a summary
(kind, slot and producer, the anchor it acknowledges or makes, finality,
proofs) above the whole of `getblock`. Everything is fetched off the GUI thread
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
  mainchain drops) only from blocks signed by the leader of their slot.
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
- With `setbmm`, while PoS blocks come, the node's merged miner makes a ticket
  per mainchain block (P is the block before its tip, or the last
  acknowledgement, or the tip itself once a ticket for the block before it
  could no longer be acknowledged in time: blocks of stake stopped for want of
  anchors), relays it, and bids for its commitment (what the pot will pay, less
  1 %); after `stall_seconds` without PoS blocks, it merged mines BMM blocks as
  the template does. It waits for its committed ticket to be acknowledged before
  it makes the next, but not for one that can no longer be.
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
  BMM block's epoch, for jailing and pruning, is taken two hours before its
  time (it may be that far ahead of the clock).
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
| `feature_fastblocks_slashing.py` | Bonded validators; a leader signs twice; the proof in a block: 10 % gone, banned, the rest unbonded; the banned validator leads no more from the next block; a proof of another of its offences takes nothing; a leader that carries the proof against itself gets no reward; a BMM block carries a proof (all validators stopped), its payee rewarded; the reward paid at the next peg step to the block that carried it; a forged proof refused; the offender out of the sets |

Fuzz targets (`src/test/fuzz/fastblocks.cpp`): `pos_header`, `pos_ticket`,
`pos_evidence`, `pos_state` (a whole chain from fuzzed choices, with bonds,
unbonds and leaders that sign twice: the state agrees with the chain key, the
totals of staking with its validators, the coins are the deposits, a tip taken
back and connected again gives the same state), `pos_staking` (the staking
rules alone with fuzzed parameters: every coin bonded is accounted for, the
journal takes any step back).

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

## Deviations from the design (phase 5)

- Leaders do not move to an anchored branch they left (§4.2): such an anchor
  is acknowledged only by leaders on that branch, and is otherwise void.
  Finality counts it against the blocks it could take back.
- (Phase 3 is built since: see Staking.)
