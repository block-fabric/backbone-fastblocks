# FastBlocks

FastBlocks is a sidechain of **eCash** (ECX): **a block every 15 seconds**. Its blocks of stake are signed by
validators who stake pegged ECX, a slot every 15 seconds, and anchored to eCash through blind merged mining
(BMM), with merged mined blocks as the fallback. A payment is *soft* in a block of stake, *BMM-confirmed* once
an eCash block commits to it, and *final* three anchors later.

It is a drivechain (BIP300/301) sidechain: ECX comes in by a deposit on eCash and goes back by a withdrawal
that eCash miners vote on. It follows its mainchain either through [Backbone](https://github.com/block-fabric/backbone)
(`backboned`, our client of the eCash networks) or through a plain eCash node with LayerTwo Labs'
`bip300301_enforcer`, over the enforcer's interface alone (`-mainchaingrpc`).

| | `ecash` (eCash main network, **the default**) | `ecashbeta` (betanet) | `ecashtest` (test network) |
|---|---|---|---|
| Slot on the mainchain | 45 | 45 | 45 |
| Coin | ECX, deposited from eCash and withdrawn back to it | the same | the same |
| Addresses | `fblx1…` | `bfblx1…` | `tfblx1…` |
| Deposit addresses | `s45_<address>_<checksum>` | the same | the same |
| P2P port | 8155 | 38155 | 48155 |
| RPC port | 8154 | 38154 | 48154 |
| Message start | `66626c21` | `66626c22` | `66626c23` |
| First block | `34f8e7a3bc8263b42e174f44c8a6cd6173acb9c323940e214ca6552e8fcdc802` | `42cb78757bb141a5817b7636c6bdad378bebed3dcb2bfcf68d4ca532173e3e09` | `2090546968fe903b34651fff7a5440fc0bc8657fd6fc4de0d8afb8e3e8cf9a29` |
| Data folder | `~/.fastblocks` | `~/.fastblocks/betanet` | `~/.fastblocks/testnet4` |

None of these networks is launched yet: slot 45 has to be proposed and activated on eCash first (below). The
main network refuses to start until this release names the height of the block that activated slot 45.

FastBlocks on eCash has the rules of FastBlocks on Chains, with an identity of its own and the parameters tied to
mainchain blocks scaled for eCash's ten-minute blocks: [doc/fastblocks.md](doc/fastblocks.md); everything
specific to eCash: [doc/ecash-sidechains.md](doc/ecash-sidechains.md) and
[doc/ecash-enforcer.md](doc/ecash-enforcer.md).

Its GUI is `fastblocks-app`: the node and its wallet with an HTML interface in a native web view (WebKitGTK on
Linux, WebView2 on Windows), with the Staking section ([doc/webui.md](doc/webui.md)).

## Building from source

On Debian or Ubuntu (24.04):

```sh
sudo apt install build-essential cmake pkgconf python3 libevent-dev libboost-dev \
    libsqlite3-dev libzmq3-dev libgtk-3-dev libwebkit2gtk-4.1-dev nodejs npm
git clone https://github.com/block-fabric/backbone-fastblocks.git
cd backbone-fastblocks
cmake -B build
cmake --build build -j$(nproc)
```

`-DBUILD_WEBUI=OFF` (and no GTK, WebKitGTK or Node.js) for a node without a window. Windows: cross-built
with the depends system (`make -C depends HOST=x86_64-w64-mingw32 NO_QT=1`), see [doc/webui.md](doc/webui.md).

The programs, in `build/bin`:

| Program | What it is |
|---|---|
| `fastblocks-app` | the wallet, with its window |
| `fastblocksd` | the node, without a window |
| `fastblocks-cli` | commands for a running node or wallet |
| `fastblocks-wallet`, `fastblocks-tx`, `fastblocks-util` | tools that work without a node |

Tests: `ctest --test-dir build` (unit tests) and `build/test/functional/test_runner.py`. The tests against a
mainchain need a built Backbone tree (`MAINCHAIN_BIN_DIR`, the folder of `backboned`; see
[doc/ecash-sidechains.md](doc/ecash-sidechains.md)).

## Running it

FastBlocks needs an eCash mainchain on the same network, in one of two ways.

**With Backbone** (`backboned`, `-chain=ecashbeta` for betanet, RPC on): FastBlocks finds it by itself, over JSON-RPC at
127.0.0.1 on Backbone's port (8532 for `ecash` and `ecashbeta`, 48532 for `ecashtest`) with the cookie in
Backbone's data folder (`~/.backbone`, `~/.backbone/betanet`, `~/.backbone/testnet4`):

```sh
fastblocks-app                       # eCash's main network, the default (-chain=ecash)
fastblocks-app -chain=ecashbeta      # betanet
fastblocksd -chain=ecashtest         # the test network, without a window
```

**With a plain eCash node and the enforcer** (ecash-com/bitcoin with `bip300301_enforcer`): FastBlocks follows
the mainchain through the enforcer's gRPC interface alone, with no JSON-RPC access to the eCash node:

```sh
fastblocksd -mainchaingrpc=127.0.0.1:50051                         # the enforcer's gRPC (its --serve-grpc-addr)
fastblocksd -chain=ecashbeta -mainchaingrpc=127.0.0.1:8123          # betanet, an enforcer on 8123
fastblocksd -mainchaingrpc=127.0.0.1:50051 -mainchaingrpcauth=<token>   # or Backbone's -grpc
```

Withdrawal bundles are handed to the enforcer's block producer once the mainchain block that committed to
them is a day deep (144 blocks; 12 on `ecashtest`). How the peg rules work from the enforcer's events alone,
and why they are safe: [doc/ecash-enforcer.md](doc/ecash-enforcer.md).

Options for the mainchain link:

| Option | What it is |
|---|---|
| `-mainchaingrpc=[http://]host:port` | follow through the enforcer's (or Backbone's) gRPC interface alone |
| `-mainchaingrpcauth=<token>` | Backbone's `-grpcauth` token (the enforcer has none) |
| `-mainchainrpcconnect=<ip>`, `-mainchainrpcport=<port>` | Backbone's JSON-RPC (default 127.0.0.1, 8532 / 48532) |
| `-mainchainrpccookiefile=<file>`, `-mainchaindatadir=<dir>` | its cookie, or the folder it is looked for in (default `~/.backbone`) |
| `-mainchainrpcuser=<user>`, `-mainchainrpcpassword=<pw>` | credentials, instead of the cookie |
| `-mainchainrpcwallet=<name>` | the Backbone wallet that pays for merged mining (default: its only loaded wallet) |

Data and the configuration file `fastblocks.conf` are in `~/.fastblocks` (Windows `%LOCALAPPDATA%\FastBlocks`,
macOS `~/Library/Application Support/FastBlocks`); betanet in its subfolder `betanet`, the test network in
`testnet4`. The app keeps its own settings in `~/.config/FastBlocks` (Windows `%LOCALAPPDATA%\FastBlocks`).
The networks have no DNS seeds: give the node a peer with `addnode`. A sample `~/.fastblocks/fastblocks.conf`:

```ini
server=1
fallbackfee=0.0002
# The network (ecash, eCash's main network, when not set):
# chain=ecashbeta

[ecashbeta]
addnode=<peer>:38155
mainchaingrpc=127.0.0.1:8123

[ecashtest]
addnode=<peer>:48155
```

## Proposing the sidechain (M1)

Slot 45 is taken on eCash by a proposal (M1) that miners ack (M2): on eCash's main network more than 1,815
acks in 2,016 blocks, on betanet more than 1,008. The proposal:

| | |
|---|---|
| Slot | 45 |
| Title | `FastBlocks` |
| Description | `A block every 15 seconds, anchored by eCash` |

With Backbone: `backbone-cli -chain=ecashbeta createsidechainproposal 45 "FastBlocks" "A block every 15 seconds, anchored by eCash"`;
with the enforcer, its `CreateSidechainProposal`. Check first that slot 45 is free (`listactivesidechains`).
Once active, the height of the activating block goes into `SidechainParams::main_activation_height` of that
network (`src/kernel/chainparams.cpp`, `CEcashSidechainParams`).

## Coins in and out

- **Deposit.** In FastBlocks, get a deposit address (`getdepositaddress`, or the **Mainchain** page):
  `s45_<address>_<checksum>`. On eCash, send to it with Backbone (`backbone-cli createsidechaindeposit 45
  <deposit address> <amount>`) or the enforcer's `CreateDepositTransaction`; or from the app's **Peg** page.
  The coins appear with the next FastBlocks block.
- **Withdraw.** `createwithdrawal <eCash address> <amount> ( <fee for eCash miners> )`, or the **Peg** page.
  Withdrawals are paid in bundles that eCash miners vote on (more than 13,150 votes in 26,300 blocks, about
  three months, on `ecash` and `ecashbeta`; more than 5 in 10 on `ecashtest`). A withdrawal not in the bundle
  being voted on can be taken back: `refundwithdrawal <txid> <vout>`.
- **Merged mining.** FastBlocks' BMM blocks are mined by eCash miners, for a fee in ECX. `setbmm true
  <FastBlocks address>` has the node ask for a block whenever the fees waiting pay for one; `requestbmmblock
  <address> <amount>` asks for one now; `getbmminfo` shows how it goes.

The node and the RPCs name eCash's coin `ECX`.

## Staking

A validator runs its node with `-validatorkeyfile=<file>` and bonds ECX to its key (`createbond`, or the
**Bonds** tab of Staking); once enough stake is bonded, blocks of stake start, a slot every 15 seconds, each
led by a validator picked by stake. The fees of a block are shared between the BMM bidder and the validator
pool by a curve of the mainchain's spacing: half each at ten-minute eCash blocks and faster, down to a fifth for
the bidder at slower ones (`getposinfo`, the Staking page's fee split). Coins are *soft* in blocks of stake,
*BMM-confirmed* once an eCash block commits to them, and *final* after three anchors (half an hour). If eCash
stalls, blocks of stake go on (soft) for up to 8 hours, and its next blocks anchor them. A bond counts in the
validator sets about ten hours after it is mined (the sets are fixed before their seeds). Unbonded coins come
back after 1008 anchors (seven days; 144 on `ecashtest`); a proof of double signing slashes them until then.
See [doc/fastblocks.md](doc/fastblocks.md) and, for the eCash values,
[doc/ecash-sidechains.md](doc/ecash-sidechains.md).

## Commands

Sidechain, on the node: `getmainchaininfo`, `getpeginfo`, `syncmainchain`, `setbmm`, `getbmminfo`,
`requestbmmblock`, `createbmmblock`, `listwithdrawals`, `getwithdrawalbundle`. In the wallet:
`getdepositaddress`, `createwithdrawal`, `refundwithdrawal`.

Staking, on the node: `getposinfo`, `getstakinginfo`, `getvalidators`, `getvalidator`, `getleaderschedule`,
`getanchorinfo`, `listevidence`, `submitevidence`, `setstaking`, `signbondpop`, `checkpegbalance`. In the
wallet: `createbond`, `createunbond`, `listbonds`.

`fastblocks-cli help` lists them all with their arguments.

## The Chains networks (development only)

FastBlocks keeps its networks on Chains (CHN: `main`, `test`, `signet`) for development, and `regtest` for the
tests. `main`, `test` and `signet` need `-chainsnetworks` (with `-chain=main|test|signet`, `-testnet` or
`-signet`), and keep their data in `~/.fastblocks/chains`. They are the identities of FastBlocks on Chains
(slot 5, `fbl1…`/`tfbl1…`): a node there joins FastBlocks' Chains networks.

## Following the sidechain template

The history of this repository continues that of FastBlocks' `ecash-enforcer` branch: the sidechain template's
eCash branch with FastBlocks' commits on it. Updates are merged as before:

```sh
git remote add template https://github.com/block-fabric/sidechain-template.git   # locally: ~/skydoge/sidechain-template
git remote add fastblocks https://github.com/block-fabric/fastblocks.git         # locally: ~/skydoge/fastblocks
git fetch template && git merge template/ecash-enforcer     # shared fixes of the eCash template
git fetch fastblocks && git merge fastblocks/main            # FastBlocks' own work on Chains
```

The eCash edition is a few commits on top (the eCash identities, `chain.json`, the default network, ECX, the
data folder); a merge brings the template's and FastBlocks' changes beside them.

## Credits

FastBlocks' fee pot is inspired by sol-drivechain (no code of it used). The eCash link follows LayerTwo Labs'
`bip300301_enforcer` interface. It is built on our sidechain template, which is based on
[Bitcoin Core](https://github.com/bitcoin/bitcoin) v32; its README is in
[doc/README-bitcoin-core.md](doc/README-bitcoin-core.md).

## License

MIT: see [COPYING](COPYING).
