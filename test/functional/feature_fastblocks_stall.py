#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""FastBlocks when the mainchain reorganizes, when blocks of stake stall, and when anchors stall.

Three validators (one key per node) and an observer, against one mainchain node:

  - a reorg of the mainchain drops the block that committed to an anchor's ticket: the
    acknowledgement and the blocks on it are undone, blocks of stake go on without it; when the
    mainchain takes the block back, so does the sidechain (one anchor more beats any number of
    blocks of stake);
  - blocks of stake stall (the validators stop): after stall_seconds BMM blocks carry the
    transactions; the validators start again, on the newest block, and anchors are tickets again;
  - anchors stall (no mainchain blocks): blocks of stake go on for max_unanchored_slots, then halt;
    when the mainchain comes back, the ticket made before is too old to be acknowledged in time,
    and a new one starts the blocks of stake again, without waiting for the BMM fallback;
  - initial block download: the observer takes in the whole chain, and comes to the same state;
  - after every step, the coins of every node are the mainchain's escrow (pegbalance).
"""

import os
import time
from decimal import Decimal

from feature_sidechain import ACTIVATION_PERIOD, SidechainTest
from test_framework.fastblocks import KEYS, SLOT, XONLY, assert_peg_balance, stop_bmm
from test_framework.util import assert_equal, assert_greater_than, rpc_port

MAX_UNANCHORED_SLOTS = 100


class FastBlocksStallTest(SidechainTest):
    def set_test_params(self):
        self.num_nodes = 4
        self.setup_clean_chain = True
        self.main_rpc_port = rpc_port(self.num_nodes)
        self.side_args = [
            f"-sidechainslot={SLOT}",
            f"-mainchainrpcport={self.main_rpc_port}",
            "-mainchainrpcuser=main",
            "-mainchainrpcpassword=secret",
            "-mainchainrpcwallet=miner",
            f"-posvalidators={','.join(XONLY)}",
        ]
        self.extra_args = [self.side_args] * self.num_nodes
        self.main_process = None

    def setup_network(self):
        # Node 3, the observer, joins at the end.
        self.start_mainchain()
        self.setup_nodes()
        self.connect_validators()

    def connect_validators(self):
        for a, b in ((0, 1), (1, 2), (0, 2)):
            self.connect_nodes(a, b)

    def validators(self):
        return self.nodes[:3]

    def wait_in_sync(self, nodes=None):
        nodes = nodes or self.validators()
        self.wait_until(lambda: len({n.getbestblockhash() for n in nodes}) == 1, timeout=60)

    def check_peg(self, nodes=None):
        assert_peg_balance(self.main, nodes or self.validators())

    def sync_main(self):
        for n in self.nodes:
            n.syncmainchain()

    def empty_main_blocks(self, count):
        """Mainchain blocks without the ticket requests of the mempool (which they make stale)."""
        hashes = [self.main.generateblock(self.main_address, [])["hash"] for _ in range(count)]
        self.sync_main()
        return hashes

    def anchor(self):
        """One more anchor: wait for the merged miner's ticket request, mine it, wait for its acknowledgement."""
        before = [n.getposinfo()["anchors"] for n in self.validators()]
        self.wait_until(lambda: len(self.main.getrawmempool()) > 0, timeout=60)
        self.mine_main()
        self.wait_until(lambda: all(n.getposinfo()["anchors"] > b for n, b in zip(self.validators(), before)), timeout=60)
        return self.main.getblockcount()

    def run_test(self):
        try:
            self.run_sidechain_test()
        finally:
            self.stop_mainchain()

    def run_sidechain_test(self):
        main = self.main
        n0, n1, n2, observer = self.nodes
        self.main_address = main.getnewaddress()

        self.log.info("Activate slot 5; a deposit in a BMM block; start the validators")
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "FastBlocks", "Blocks of stake between merged mined blocks", "11" * 32, "22" * 20)
        self.mine_main(ACTIVATION_PERIOD + 1)
        main.createsidechaindeposit(SLOT, n0.getdepositaddress()["depositaddress"], 50)
        self.mine_main()
        block = n0.createbmmblock(n0.getnewaddress())
        main.createbmmrequest(SLOT, block["hash"], Decimal("0.001"))
        self.mine_main()
        self.wait_until(lambda: all(n.getbestblockhash() == block["hash"] for n in self.validators()))
        self.check_peg()
        for i, key in enumerate(KEYS):
            path = os.path.join(self.options.tmpdir, f"validator{i}.key")
            with open(path, "w") as f:
                f.write(key.hex() + "\n")
            self.restart_node(i, extra_args=self.side_args + [f"-validatorkeyfile={path}"])
        self.connect_validators()
        self.wait_until(lambda: all(n.getstakinginfo()["produced"] > 0 for n in self.validators()), timeout=120)
        for n in (n1, n2):
            txid = n0.sendtoaddress(n.getnewaddress(), 10)
            self.wait_until(lambda: n0.gettransaction(txid)["confirmations"] > 0, timeout=60)
        ticket_address = n0.getnewaddress()
        n0.setbmm(True, ticket_address, True, Decimal("0.0001"))
        self.anchor()
        self.wait_in_sync()
        self.check_peg()

        self.log.info("A reorg of the mainchain drops the commitment of an anchor: the sidechain undoes it")
        committed = self.anchor()
        self.wait_in_sync()
        acked = n0.getanchorinfo()["recent"][0]
        assert_equal(acked["mainheight"], committed)
        stop_bmm(main, self.main_address, n0, self.nodes)
        # The block that committed, and two more after it (none commits to anything).
        self.empty_main_blocks(2 - (main.getblockcount() - committed))
        dropped = main.getblockhash(committed)
        main.invalidateblock(dropped)
        # A shorter branch of the mainchain in its place.
        self.empty_main_blocks(2)
        assert_equal(main.getblockcount(), committed + 1)
        self.wait_until(lambda: all(n.getblock(acked["block"])["confirmations"] == -1 for n in self.validators()), timeout=60)
        self.wait_until(lambda: all(n.getposinfo()["lastanchormainheight"] < committed for n in self.validators()), timeout=60)
        # Blocks of stake go on without it.
        height = n0.getblockcount()
        self.wait_until(lambda: n0.getblockcount() > height + 2, timeout=60)
        self.wait_in_sync()
        self.check_peg()

        self.log.info("The mainchain takes the block back: so does the sidechain")
        main.reconsiderblock(dropped)
        assert_equal(main.getblockhash(committed), dropped)
        self.sync_main()
        self.wait_until(lambda: all(n.getblock(acked["block"])["confirmations"] > 0 for n in self.validators()), timeout=60)
        self.wait_until(lambda: all(n.getposinfo()["lastanchormainheight"] == committed for n in self.validators()), timeout=60)
        self.wait_in_sync()
        self.check_peg()

        self.log.info("Blocks of stake stall: after stall_seconds, BMM blocks carry the transactions")
        for n in self.validators():
            n.setstaking(False)
        self.wait_in_sync()
        last_pos = n0.getposinfo()["lastposslot"]
        txid = n1.sendtoaddress(n2.getnewaddress(), 2)
        n0.setbmm(True, ticket_address, True, Decimal("0.0001"))
        # The mainchain's median time has to pass the last block of stake by stall_seconds.
        deadline = time.time() + 180
        while not n0.getposinfo()["stalled"]:
            assert time.time() < deadline, "no stall"
            self.mine_main()
            time.sleep(1)
        self.sync_mempools(self.validators())
        deadline = time.time() + 120
        while n1.gettransaction(txid)["confirmations"] == 0:
            assert time.time() < deadline, "the transaction did not confirm"
            self.mine_main()
            time.sleep(1)
        bmm_block = n1.gettransaction(txid)["blockhash"]
        assert_equal(n0.getblock(bmm_block)["kind"], "bmm")
        assert_equal(n0.getposinfo()["lastposslot"], last_pos)
        self.wait_in_sync()
        self.check_peg()

        self.log.info("The validators start again, on the newest block; anchors are tickets again")
        stop_bmm(main, self.main_address, n0, self.nodes)
        bmm_height = n0.getblockheader(n0.getbestblockhash())["height"]
        for n in self.validators():
            n.setstaking(True)
        self.wait_until(lambda: n0.getblock(n0.getbestblockhash())["kind"] == "pos" and n0.getblockcount() > bmm_height, timeout=60)
        assert_greater_than(n0.getposinfo()["lastposslot"], last_pos)
        n0.setbmm(True, ticket_address, True, Decimal("0.0001"))
        self.anchor()
        assert_equal(n0.getanchorinfo()["recent"][0]["kind"], "ticket")
        self.wait_in_sync()
        self.check_peg()

        self.log.info("Anchors stall: blocks of stake halt after max_unanchored_slots")
        # The merged miner's request for the mainchain tip waits in the mempool; no mainchain block comes.
        self.wait_until(lambda: len(main.getrawmempool()) > 0, timeout=60)
        first_unanchored = n0.getposinfo()["slot"]
        self.wait_until(lambda: any("no anchor for too long" in n.getposinfo()["producer"]["error"] for n in self.validators()),
                        timeout=MAX_UNANCHORED_SLOTS + 60)
        halted = n0.getblockcount()
        assert_greater_than(n0.getposinfo()["slot"], first_unanchored + MAX_UNANCHORED_SLOTS - 10)
        self.wait_in_sync()
        # (No block for a few slots.)
        slot = n0.getposinfo()["slot"]
        self.wait_until(lambda: n0.getposinfo()["slot"] > slot + 3, timeout=30)
        assert_equal(n0.getblockcount(), halted)
        self.check_peg()

        self.log.info("The mainchain comes back: a new ticket, in time, starts the blocks of stake again")
        anchors = n0.getposinfo()["anchors"]
        stale = main.getblockcount() + 1
        # The block commits to the ticket made before the halt, which no leader can acknowledge in time.
        self.mine_main()
        # The merged miner does not wait for it: a ticket for a block that can still be anchored.
        self.wait_until(lambda: len(main.getrawmempool()) > 0, timeout=60)
        self.mine_main()
        self.wait_until(lambda: n0.getposinfo()["anchors"] > anchors, timeout=60)
        self.wait_until(lambda: n0.getblockcount() > halted + 2, timeout=60)
        recent = n0.getanchorinfo()["recent"][0]
        assert_equal(recent["kind"], "ticket")
        assert_equal(recent["mainheight"], stale + 1)
        assert all(a["mainheight"] != stale for a in n0.getanchorinfo(20)["recent"])
        # Not a BMM block: blocks of stake went on without one.
        assert all(n0.getblock(n0.getblockhash(h))["kind"] == "pos" for h in range(halted - 3, n0.getblockcount() + 1))
        stop_bmm(main, self.main_address, n0, self.nodes)
        self.wait_in_sync()
        self.check_peg()

        self.log.info("Initial block download: the observer takes in the whole chain")
        for n in self.validators():
            n.setstaking(False)
        self.wait_in_sync()
        assert_equal(observer.getblockcount(), 0)
        for i in range(3):
            self.connect_nodes(3, i)
        self.wait_in_sync(self.nodes)
        states = [n.getsidechainstate() for n in self.nodes]
        assert_equal(len({(st["bestblock"], st["hash"]) for st in states}), 1)
        assert_equal(observer.getanchorinfo()["recent"], n0.getanchorinfo()["recent"])
        self.check_peg(self.nodes)


if __name__ == '__main__':
    FastBlocksStallTest(__file__).main()
