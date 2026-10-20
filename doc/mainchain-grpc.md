# The mainchain over the bip300301_enforcer's interface

A sidechain node of this template follows its mainchain node over JSON-RPC (`-mainchainrpcport`): Backbone
on the eCash networks, Chains on the Chains networks. Backbone also serves the bip300301_enforcer's interface
(gRPC, gRPC-Web and the Connect protocol on one port, `-grpcport`, default 50051; Backbone's
`doc/ecash-grpc.md`), which the LayerTwo sidechains use. This is what that interface can do for this template.

## What is implemented (branch `ecash-grpc`)

`-mainchaingrpcport=<port>` (and `-mainchaingrpcauth=<token>`, Backbone's `-grpcauth`): this node's
**requests** to the mainchain go through the enforcer's interface, with the Connect protocol (JSON over
HTTP/1.1, `MainClient::CallEnforcer`: no HTTP/2 or protobuf library needed); **following** stays on JSON-RPC.
JSON-RPC stays the default for everything.

| What the node does | JSON-RPC (default) | With -mainchaingrpcport |
|---|---|---|
| Follow the mainchain, block by block | `getsidechainevents`, `getblockchaininfo`, `getblockcount` | the same (below) |
| Check its slot | `getsidechain` | the same |
| Ask for blind merged mining | `createbmmrequest` (wallet) | `ValidatorService.GetChainTip` (the tip must be the block the commitment is for), `WalletService.CreateBmmCriticalDataTransaction`; INVALID_ARGUMENT naming `prev_bytes` read as "the mainchain moved", "Outbid:" kept |
| Hand over a withdrawal bundle | `receivewithdrawalbundle` | `BlockProducerService.ProposeWithdrawalBundle` (the blind bundle, legacy serialization, base64) |
| Vouch for its bundle | `vouchwithdrawalbundle slot hash` | the hand-off itself: the block producer's KNOWN policy (Backbone's default) upvotes the bundles it was handed. `SetWithdrawalBundleAck` is not used: it is the operator's vote, over any policy (with it, an operator's "abstain" on the slot would be overridden) |
| Vouch for none (downvote the slot's others) | `vouchwithdrawalbundle slot` | nothing: the interface has no word of a sidechain, nor a per-sidechain alarm (its ALARM policy is for all sidechains) |
| Broadcast the payout once the bundle has its votes | `sendwithdrawalbundle` | nothing: the block producer that holds the bundle pays it out (M6) itself |
| Mainchain addresses of withdrawals | `validateaddress` | the same |

A limit of the interface: this node cannot take its word back. Over JSON-RPC, a bundle the chain gave up (a
reorg of the sidechain, another bundle of the slot paid) stops being upvoted at once (`vouchwithdrawalbundle
slot`); over the enforcer's interface the block producer keeps upvoting the last bundle it was handed until a
new one is handed or the bundle closes. One reason JSON-RPC stays the default.

With Backbone the two ports are one node. Tested by `feature_sidechain_ecash.py --mainchaingrpc` (Backbone's
`-grpc -grpcauth` on regtest `-testecash`): BMM blocks requested by the sidechain node itself, the deposit, a
withdrawal proposed, voted and paid, a failed bundle refunded, the reorg case.

## Why following stays on JSON-RPC

The template's consensus needs, for every mainchain block, the withdrawal bundles of its slot pending after it
**with their scores** (`getsidechainevents` "pending": refund gating and the pending bundle rules,
`sidechain/state.cpp`, `pending_min_score`). The enforcer's interface reports, per block, what was proposed
(Submitted), failed and paid (Succeeded), deposits and the BMM commitment (`GetBlockInfo`,
`SubscribeEvents`, `GetTwoWayPegData`), but scores only as they are now (`GetWithdrawalBundleProposals`).
Two nodes deriving the scores of past blocks differently would fork the sidechain, so a follower over the
enforcer's interface alone needs one of:

1. **Replay the votes** from the blocks (M3/M4 by the enforcer's rules: `handle_m4_ack_bundles`,
   `LeadingBy50`, `RepeatPrevious`, the failure step), from raw blocks of a plain eCash node (`getblock` 2):
   ~600 lines, consensus-critical, and it needs the active sidechains in slot order at every height (M4
   entries are by position), which `GetSidechains` gives only for now (a slot replaced in the past is lost).
2. **The enforcer reporting them**: per-block pending m6ids with vote counts in `BlockInfo` (a new field), or
   `GetWithdrawalBundleProposals` at a block hash. Asked of LayerTwo Labs (audit 2026-10-19, open question 3).
3. **Backbone's JSON-RPC** for following, which is what this branch does; with bitcoind + enforcer as the
   mainchain, the sidechain node then cannot run (bitcoind has no `getsidechainevents`).

The rest of a full gRPC mode is mechanical (≈1,500 lines over the follower's calls):

| Follower need | Over the enforcer's interface |
|---|---|
| `getblockchaininfo`, `getblockcount` (sync, IBD, reorg) | `GetChainTip`, `GetBlockHeaderInfo` (ancestors) |
| per-block hash, previous hash, time | `BlockHeaderInfo` (no median time: `GetBlockHeaderInfo` of the 11 blocks before, computed) |
| `bmm` | `BlockInfo.bmm_commitment` (ConsensusHex: bytes in order) |
| deposits (`destination`, `amount`, `txid`, `burnindex`) | `Deposit` events (`output.address` hex of the destination bytes, `value_sats` what was added, `outpoint`) |
| withdrawal change ("D", bundle, payouts) | `Succeeded` (the M6 transaction: output 0 the new treasury, the payouts after) |
| `bundles` closed (`paid`), `proposed` | `Succeeded`, `Failed`, `Submitted` |
| `pending` with scores | **not available** (above) |
| `getsidechain` (activation height, proposal hash) | `GetSidechains` (`activation_height`, sha256d of `description`) |
| stream instead of polling | `SubscribeEvents` (ConnectBlock / DisconnectBlock) |

Transport: the Connect protocol (unary, JSON) over this template's HTTP/1.1 client is enough for unary calls;
`SubscribeEvents` would need Connect's streaming form (`application/connect+json`, enveloped messages over a
chunked HTTP/1.1 reply), which Backbone and the enforcer serve; polling `GetBlockInfo` with `max_ancestors` is the
simpler start.
