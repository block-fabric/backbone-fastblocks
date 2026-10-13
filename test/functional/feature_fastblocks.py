#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""FastBlocks with three validators on regtest (one key per node), against a mainchain node.

  - before the validators start, the chain is merged mined (BMM blocks, a deposit);
  - the validators make a block of stake per slot (1 s on regtest), each signed by
    the leader of its slot, and the nodes agree on them;
  - the merged miner makes a ticket per mainchain block; the mainchain commits to it;
    a leader acknowledges it: the anchors count up on every node;
  - a block of stake splits its fees: half to its producer, half to the pot, which
    the next acknowledgement pays to the ticket's payee;
  - a withdrawal made in a block of stake goes into a bundle at an acknowledgement;
  - slashing protection: the slots signed are on disk, next to the key file, and a second
    node with the same key file does not start;
  - without -validatorpayout, the producer's share goes to a new address of the node's wallet;
  - without blocks of stake for stall_seconds, BMM blocks carry transactions again.
"""

import os
import time
from decimal import Decimal

from feature_sidechain import ACTIVATION_PERIOD, SidechainTest
from test_framework.fastblocks import assert_peg_balance
from test_framework.key import compute_xonly_pubkey
from test_framework.test_node import ErrorMatch
from test_framework.util import assert_equal, assert_greater_than, rpc_port

SLOT = 5
POS_BLOCK_BIT = 0x10000
POT_SCRIPT = "6a04de504f54"
STALL_SECONDS = 30
KEYS = [bytes([0x11] + [0] * 30 + [i]) for i in (1, 2, 3)]


class FastBlocksTest(SidechainTest):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        self.main_rpc_port = rpc_port(self.num_nodes)
        validators = ",".join(compute_xonly_pubkey(key)[0].hex() for key in KEYS)
        self.side_args = [
            f"-sidechainslot={SLOT}",
            f"-mainchainrpcport={self.main_rpc_port}",
            "-mainchainrpcuser=main",
            "-mainchainrpcpassword=secret",
            "-mainchainrpcwallet=miner",
            f"-posvalidators={validators}",
        ]
        self.extra_args = [self.side_args] * self.num_nodes
        self.main_process = None

    def key_file(self, i):
        return os.path.join(self.options.tmpdir, f"validator{i}.key")

    def validator_args(self, i, key_index=None):
        key_index = i if key_index is None else key_index
        return self.side_args + [f"-validatorkeyfile={self.key_file(key_index)}", f"-validatorpayout={self.payouts[i]}"]

    def connect_all(self):
        for a, b in ((0, 1), (1, 2), (0, 2)):
            self.connect_nodes(a, b)

    def bmm(self, node, address):
        """A BMM block by hand: build it, have the mainchain commit to it."""
        block = node.createbmmblock(address)
        self.main.createbmmrequest(SLOT, block["hash"], Decimal("0.001"))
        self.mine_main()
        self.wait_until(lambda: all(n.getbestblockhash() == block["hash"] for n in self.nodes))
        return block["hash"]

    def is_pos(self, node, blockhash):
        return bool(node.getblockheader(blockhash)["version"] & POS_BLOCK_BIT)

    def wait_in_sync(self):
        self.wait_until(lambda: len({n.getbestblockhash() for n in self.nodes}) == 1, timeout=60)

    def anchor(self):
        """One more anchor: wait for the merged miner's ticket request, mine it, wait for its acknowledgement."""
        before = [n.getposinfo()["anchors"] for n in self.nodes]
        self.wait_until(lambda: len(self.main.getrawmempool()) > 0, timeout=60)
        self.mine_main()
        self.wait_until(lambda: all(n.getposinfo()["anchors"] > b for n, b in zip(self.nodes, before)), timeout=60)

    def run_test(self):
        try:
            self.run_sidechain_test()
        finally:
            self.stop_mainchain()

    def run_sidechain_test(self):
        main = self.main
        n0, n1, n2 = self.nodes
        self.main_address = main.getnewaddress()

        self.log.info("Activate slot 5 on the mainchain")
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "FastBlocks", "Blocks of stake between merged mined blocks", "11" * 32, "22" * 20)
        self.mine_main(ACTIVATION_PERIOD + 1)
        assert_equal([s["slot"] for s in main.listactivesidechains()], [SLOT])

        self.log.info("Before the validators start: merged mined blocks, a deposit")
        info = n0.getposinfo()
        assert_equal(info["active"], True)
        assert_equal(len(info["validators"]), 3)
        assert_equal(info["lastposslot"], -1)
        assert_equal(info["stalled"], True)
        address0 = n0.getnewaddress()
        main.createsidechaindeposit(SLOT, n0.getdepositaddress()["depositaddress"], 50)
        self.mine_main()
        first = self.bmm(n0, address0)
        assert not self.is_pos(n0, first)
        assert_equal(n0.getbalance(), 50)
        assert_equal(n0.getposinfo()["anchors"], 1)
        assert_peg_balance(main, self.nodes)

        self.log.info("Start the validators: a block of stake per slot, by its leader")
        self.payouts = [n.getnewaddress() for n in self.nodes]
        for i, key in enumerate(KEYS):
            with open(self.key_file(i), "w") as f:
                f.write("# validator key\n" + key.hex() + "\n")
        for i in range(self.num_nodes):
            self.restart_node(i, extra_args=self.validator_args(i))
        self.connect_all()
        height = n0.getblockcount()
        self.wait_until(lambda: all(n.getblockcount() >= height + 6 for n in self.nodes), timeout=60)
        self.wait_in_sync()
        tip = n0.getbestblockhash()
        assert self.is_pos(n0, tip)
        # Every validator leads some slots.
        self.wait_until(lambda: all(n.getposinfo()["producer"]["produced"] > 0 for n in self.nodes), timeout=120)
        info = n0.getposinfo()
        assert_equal(info["producer"]["keys"], [compute_xonly_pubkey(KEYS[0])[0].hex()])
        assert info["lastposslot"] > 0
        # What each validator signed is on disk before its blocks went out, next to its key file.
        for i, node in enumerate(self.nodes):
            with open(self.key_file(i) + ".protection") as f:
                lines = f.read().splitlines()
            assert_equal(lines[0], "fastblocks-slashing-protection 1")
            assert_equal(lines[1].split()[0], compute_xonly_pubkey(KEYS[i])[0].hex())

        self.log.info("Anchors: tickets the mainchain commits to, acknowledged by a leader")
        ticket_address = n0.getnewaddress()
        n0.setbmm(True, ticket_address, True, Decimal("0.0001"))
        for _ in range(2):
            self.anchor()
        self.wait_in_sync()
        info = n0.getposinfo()
        assert_equal(info["lastanchormainheight"], main.getblockcount())
        assert_greater_than(info["anchors"], 2)
        assert_greater_than(info["tickets"], 0)
        # Every node knows the tickets (relayed).
        self.wait_until(lambda: all(n.getposinfo()["tickets"] >= 2 for n in self.nodes))
        assert_peg_balance(main, self.nodes)

        self.log.info("The fee split: half to the producer, half to the pot")
        txid = n0.sendtoaddress(n1.getnewaddress(), 1)
        self.wait_until(lambda: n0.gettransaction(txid)["confirmations"] > 0, timeout=30)
        tx = n0.gettransaction(txid)
        fee = -tx["fee"]
        block = n0.getblock(tx["blockhash"], 2)
        assert self.is_pos(n0, tx["blockhash"])
        coinbase = block["tx"][0]
        sats = int(fee * 100_000_000)
        assert_equal(int(coinbase["vout"][0]["value"] * 100_000_000), sats // 2)
        pots = [out for out in coinbase["vout"] if out["scriptPubKey"]["hex"] == POT_SCRIPT]
        assert_equal(len(pots), 1)
        assert_equal(int(pots[0]["value"] * 100_000_000), sats - sats // 2)
        self.wait_until(lambda: n0.getposinfo()["pot"]["accrued"] >= pots[0]["value"])
        # The next acknowledgements pay it to the ticket's payee.
        received = n0.getreceivedbyaddress(ticket_address, 0)
        # (The anchorer bids for every mainchain block as soon as it sees it: a ticket may anchor a
        # block from before the one with the fee, and the next one pays it.)
        for _ in range(5):
            self.anchor()
            if n0.getposinfo()["pot"]["paid"] == n0.getposinfo()["pot"]["accrued"]:
                break
        self.wait_until(lambda: n0.getposinfo()["pot"]["paid"] == n0.getposinfo()["pot"]["accrued"], timeout=60)
        assert_equal(n0.getreceivedbyaddress(ticket_address, 0) - received, pots[0]["value"])
        self.wait_in_sync()
        assert_peg_balance(main, self.nodes)

        self.log.info("A withdrawal in a block of stake goes into a bundle at an acknowledgement")
        withdrawal = n0.createwithdrawal(main.getnewaddress(), 3, Decimal("0.01"))
        self.wait_until(lambda: n0.gettransaction(withdrawal["txid"])["confirmations"] > 0, timeout=30)
        assert_equal(n0.getwithdrawalbundle()["status"], "next")
        # (A ticket made before the last commitment was acknowledged builds no bundle: once that
        # acknowledgement is in, the anchorer makes its ticket again for a block after it, a moment
        # later; mainchain blocks come minutes apart.)
        for _ in range(6):
            time.sleep(3)
            self.anchor()
            if n0.getwithdrawalbundle()["status"] != "next":
                break
        self.wait_in_sync()
        bundle = n0.getwithdrawalbundle()
        assert bundle["status"] != "next", bundle
        assert_equal(len({str(n.getwithdrawalbundle()) for n in self.nodes}), 1)
        # The withdrawal's coins are burned on this chain and still in escrow.
        assert_equal(assert_peg_balance(main, self.nodes)["withdrawals"], Decimal("3.01"))

        self.log.info("Slashing protection: a second node with the same keys does not start")
        self.stop_node(2)
        # The slots signed are kept next to the key file (wiping the data directory keeps them).
        protection = self.key_file(2) + ".protection"
        assert not os.path.exists(n2.chain_path / "validator_protection.dat")
        # (What it signed before it stopped: read now, as it may sign again as soon as it starts.)
        with open(protection) as f:
            signed_before = int(f.read().splitlines()[1].split()[1])
        self.nodes[2].assert_start_raises_init_error(self.validator_args(2, key_index=0), expected_msg="FastBlocks block producer: cannot lock", match=ErrorMatch.PARTIAL_REGEX)
        # Without -validatorpayout: the producer's share goes to a new address of the node's wallet.
        self.start_node(2, extra_args=self.side_args + [f"-validatorkeyfile={self.key_file(2)}"])
        self.connect_nodes(0, 2)
        self.connect_nodes(1, 2)
        self.wait_in_sync()
        # A restarted validator goes on, after the slots it signed before.
        self.wait_until(lambda: n2.getposinfo()["producer"]["produced"] > 0, timeout=60)
        self.wait_in_sync()
        with open(protection) as f:
            assert_greater_than(int(f.read().splitlines()[1].split()[1]), signed_before)
        payout = n2.getstakinginfo()["payout"]
        info = n2.getaddressinfo(payout)
        assert info["ismine"]
        assert_equal(info["labels"], ["validator payout"])
        def paid():
            if n2.getreceivedbyaddress(payout, 0) > 0:
                return True
            # Fees for its blocks to share.
            n0.sendtoaddress(n1.getnewaddress(), Decimal("0.1"))
            time.sleep(1)
            return False
        self.wait_until(paid, timeout=120)
        assert any(t.get("income") == "staked" and t["address"] == payout for t in n2.listtransactions("*", 1000))
        self.wait_in_sync()

        self.log.info("Without blocks of stake for stall_seconds, BMM blocks carry transactions")
        for i in range(self.num_nodes):
            self.restart_node(i, extra_args=self.side_args)
        self.connect_all()
        self.wait_in_sync()
        last_pos = n0.getposinfo()["lastposslot"]
        n0.setbmm(True, ticket_address, True, Decimal("0.0001"))
        txid = n0.sendtoaddress(n1.getnewaddress(), 2)
        # The mainchain's median time has to pass the last block of stake by stall_seconds; then the
        # merged miner makes BMM blocks again, with the transactions.
        deadline = time.time() + STALL_SECONDS * 4
        while not n0.getposinfo()["stalled"]:
            assert time.time() < deadline, "no stall"
            self.mine_main()
            time.sleep(1)
        self.sync_mempools()
        deadline = time.time() + 60
        while n0.gettransaction(txid)["confirmations"] == 0:
            assert time.time() < deadline, "the transaction did not confirm"
            self.mine_main()
            time.sleep(1)
        blockhash = n0.gettransaction(txid)["blockhash"]
        assert not self.is_pos(n0, blockhash)
        assert_equal(n0.getposinfo()["lastposslot"], last_pos)
        self.wait_in_sync()
        assert_peg_balance(main, self.nodes)


if __name__ == '__main__':
    FastBlocksTest(__file__).main()
