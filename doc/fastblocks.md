# FastBlocks

FastBlocks is a sidechain of Chains (CHN) in slot 5. It is a hybrid chain:
*PoS blocks* every 30 seconds, signed by validators who stake pegged CHN, and
*BMM blocks*, the template's blind merged mined blocks, as the fallback. The
mainchain anchors the PoS chain through BMM commitments to *tickets*. The full
design is `~/skydoge/design/fastblocks.md`; this file says what is built.

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
