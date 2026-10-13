#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""FastBlocks across restarts: what a node keeps in memory is rebuilt from its state, or from its peers,
or kept next to the key. Three bonded validators (no -posvalidators) and an observer that merged mines:

  - a validator restarted, before any anchor comes: a BMM block 30 minutes ahead is refused, as the
    other nodes refuse it (the 10 minute limit of a network with validators is the tip's state's);
  - every validator restarted while a committed ticket waits for its acknowledgement: they ask the
    observer for it, and acknowledge it;
  - a validator's data directory wiped, its key kept: it does not sign again a slot it signed (the
    slashing protection is next to the key file), and goes on making blocks;
  - a validator killed between signing a block and relaying it (-posfaultaftersign): restarted, it does
    not sign that slot again, and goes on making blocks.
"""

import os
import shutil
import time
from decimal import Decimal

from feature_sidechain import SidechainTest
from test_framework.fastblocks import (
    REGTEST_GENESIS_TIME,
    SLOT,
    XONLY,
    activate_and_fund,
    assert_peg_balance,
    bond_validators,
    stop_bmm,
    write_key_file,
)
from test_framework.util import assert_equal, assert_greater_than, p2p_port, rpc_port

#: A BMM block this far ahead of the clock: within Bitcoin's two hours, past the ten minutes of a
#: network with validators.
AHEAD = 30 * 60


def slot_time(slot):
    return REGTEST_GENESIS_TIME + slot


class FastBlocksRestartTest(SidechainTest):
    def set_test_params(self):
        self.num_nodes = 4
        self.setup_clean_chain = True
        self.main_rpc_port = rpc_port(self.num_nodes)
        # No -posvalidators: the validators bond.
        self.side_args = [
            f"-sidechainslot={SLOT}",
            f"-mainchainrpcport={self.main_rpc_port}",
            "-mainchainrpcuser=main",
            "-mainchainrpcpassword=secret",
            "-mainchainrpcwallet=miner",
        ]
        self.extra_args = [self.side_args] * self.num_nodes
        self.main_process = None

    def setup_network(self):
        self.start_mainchain()
        self.setup_nodes()
        self.connect_all()

    def connect_all(self):
        for a, b in ((0, 1), (1, 2), (0, 2), (3, 0), (3, 1), (3, 2)):
            self.connect_nodes(a, b)

    def connect_to_all(self, i):
        """Connect node `i` (restarted) to the others."""
        for other in range(self.num_nodes):
            if other != i:
                self.connect_nodes(i, other)

    def mine_main(self, count=1):
        hashes = self.main.generatetoaddress(count, self.main_address)
        for node in self.nodes:
            if node.running:
                node.syncmainchain()
        return hashes

    def validators(self):
        return self.nodes[:3]

    def validator_args(self, i):
        return self.side_args + [f"-validatorkeyfile={self.key_files[i]}", f"-validatorpayout={self.payouts[i]}"]

    def wait_in_sync(self, nodes=None, timeout=90):
        nodes = nodes or self.nodes
        self.wait_until(lambda: len({n.getbestblockhash() for n in nodes}) == 1, timeout=timeout)

    def anchor(self):
        """One more anchor: mine the merged miner's ticket requests until one is acknowledged."""
        before = [n.getposinfo()["anchors"] for n in self.validators()]

        def anchored():
            if all(n.getposinfo()["anchors"] > b for n, b in zip(self.validators(), before)):
                return True
            if self.main.getrawmempool():
                self.mine_main()
            time.sleep(0.5)
            return False
        self.wait_until(anchored, timeout=120)

    def protected_slot(self, i):
        """The slot the slashing protection next to validator i's key file has (written when it signs)."""
        with open(self.key_files[i] + ".protection", encoding="utf-8") as f:
            lines = f.read().splitlines()
        assert_equal(lines[0], "fastblocks-slashing-protection 1")
        slots = dict(line.split() for line in lines[1:])
        return int(slots[XONLY[i]])

    def slots_on_chain(self, node, since_slot):
        """The slots of the blocks of stake on node's active chain, from a slot on."""
        slots = set()
        block = node.getblock(node.getbestblockhash())
        while True:
            if block["kind"] == "pos":
                if block["slot"] < since_slot:
                    break
                slots.add(block["slot"])
            if "previousblockhash" not in block:
                break
            block = node.getblock(block["previousblockhash"])
        return slots

    def assert_no_evidence(self):
        for n in self.nodes:
            assert_equal(n.listevidence(), [])

    def run_test(self):
        try:
            self.run_sidechain_test()
        finally:
            self.stop_mainchain()

    def run_sidechain_test(self):
        main = self.main
        n0, n1, n2, observer = self.nodes
        self.main_address = main.getnewaddress()
        self.observer_address = observer.getnewaddress()

        self.log.info("Activate slot 5, a deposit; three validators bond, blocks of stake start")
        activate_and_fund(self, funded=0)
        self.key_files = [write_key_file(self, i) for i in range(3)]
        self.payouts = [n.getnewaddress() for n in self.validators()]
        for i in range(3):
            self.restart_node(i, extra_args=self.validator_args(i))
        self.connect_all()
        bond_validators(self, n0, range(3))
        self.wait_until(lambda: all(n.getstakinginfo()["produced"] > 0 for n in self.validators()), timeout=180)
        self.wait_in_sync()
        observer.setbmm(True, self.observer_address, True, Decimal("0.0001"))
        self.anchor()
        stop_bmm(main, self.main_address, observer, [n for n in self.nodes])
        self.wait_in_sync()
        assert_peg_balance(main, self.nodes)

        self.log.info("A validator restarted, no anchor since: a BMM block 30 minutes ahead is refused, as the others refuse it")
        anchors = n2.getposinfo()["anchors"]
        self.restart_node(2, extra_args=self.validator_args(2))
        self.connect_to_all(2)
        # The observer makes the block, its clock ahead, and keeps it to itself.
        for i in range(3):
            self.disconnect_nodes(3, i)
        observer.setmocktime(int(time.time()) + AHEAD)
        block = observer.createbmmblock(self.observer_address)
        main.createbmmrequest(SLOT, block["hash"], Decimal("0.001"))
        self.mine_main()
        self.wait_until(lambda: observer.getbestblockhash() == block["hash"], timeout=30)
        raw = observer.getblock(block["hash"], 0)
        observer.invalidateblock(block["hash"])
        observer.setmocktime(0)
        assert_equal(n2.getposinfo()["anchors"], anchors)
        for n in (n2, n0):
            assert_equal(n.submitblock(raw), "time-too-new")
        self.wait_in_sync(self.validators())
        assert block["hash"] not in {t["hash"] for t in n2.getchaintips() if t["status"] == "active"}
        for i in range(3):
            self.connect_nodes(3, i)
        self.wait_in_sync()
        assert_peg_balance(main, self.nodes)

        self.log.info("Every validator restarted while a committed ticket waits: they ask the observer for it, and acknowledge it")
        for n in self.validators():
            n.setstaking(False)
        observer.setbmm(True, self.observer_address, True, Decimal("0.0001"))
        self.wait_until(lambda: len(main.getrawmempool()) > 0, timeout=60)
        self.mine_main()
        committed = main.getblockcount()
        observer.setbmm(False)
        ticket = [t for t in observer.getanchorinfo()["tickets"] if t.get("committedheight") == committed]
        assert_equal(len(ticket), 1)
        for i in range(3):
            self.restart_node(i, extra_args=self.validator_args(i))
        self.connect_all()
        for n in self.validators():
            assert ticket[0]["hash"] not in {t["hash"] for t in n.getanchorinfo()["tickets"]}
        # At the next mainchain block, the validators ask their peers for the tickets of the
        # commitments they have neither a ticket nor a block of.
        self.mine_main()
        self.wait_until(lambda: all(any(a["mainheight"] == committed for a in n.getanchorinfo()["recent"]) for n in self.validators()), timeout=60)
        self.wait_in_sync()
        assert_peg_balance(main, self.nodes)

        self.log.info("A validator's data directory wiped, its key kept: it does not sign a slot it signed again")
        self.wait_until(lambda: n2.getstakinginfo()["produced"] > 0, timeout=60)
        last = n2.getstakinginfo()["lastsignedslot"]
        assert_equal(self.protected_slot(2), last)
        own = n0.getbestblockhash()
        while n0.getblock(own)["kind"] != "pos" or n0.getblock(own)["slot"] > last:
            own = n0.getblock(own)["previousblockhash"]
        assert_equal(n0.getblock(own)["slot"], last)
        below = n0.getblock(own)["previousblockhash"]
        self.stop_node(2)
        shutil.rmtree(os.path.join(n2.datadir_path, self.chain))
        # Its clock just before that slot: it takes in the chain below its block, not the block (a slot
        # to come), and is then cut off.
        self.start_node(2, extra_args=self.validator_args(2) + [f"-mocktime={slot_time(last) - 3}"])
        self.connect_nodes(2, 0)
        self.connect_nodes(2, 1)
        self.wait_until(lambda: n2.getbestblockhash() == below, timeout=60)
        self.disconnect_nodes(2, 0)
        self.disconnect_nodes(2, 1)
        with n2.assert_debug_log([f"no block for slot {last}: slashing protection"], unexpected_msgs=[f"of slot {last}"], timeout=20):
            n2.setmocktime(slot_time(last))
        n2.setmocktime(0)
        self.connect_to_all(2)
        produced = n2.getstakinginfo()["produced"]
        self.wait_until(lambda: n2.getstakinginfo()["produced"] > produced, timeout=60)
        self.wait_in_sync()
        assert_equal(n0.getblock(own)["confirmations"] > 0, True)
        self.assert_no_evidence()
        assert_peg_balance(main, self.nodes)

        self.log.info("A validator killed between signing a block and relaying it: restarted, it does not sign that slot again")
        # (Its peers from the start: it may stop before an RPC could connect it.)
        self.restart_node(1, extra_args=self.validator_args(1) + ["-posfaultaftersign"] + [f"-addnode=127.0.0.1:{p2p_port(i)}" for i in (0, 2, 3)])
        n1.wait_until_stopped(expected_ret_code=1, timeout=60)
        signed = self.protected_slot(1)
        with open(n1.debug_log_path, encoding="utf-8") as log:
            assert f"the block of slot {signed} is signed; the node stops at once" in log.read()
        self.start_node(1, extra_args=self.validator_args(1))
        self.connect_to_all(1)
        produced = n1.getstakinginfo()["produced"]
        self.wait_until(lambda: n1.getstakinginfo()["produced"] > produced, timeout=60)
        assert_greater_than(n1.getstakinginfo()["lastsignedslot"], signed)
        self.wait_in_sync()
        # Nobody has a block of that slot: it was signed, neither stored nor relayed, and not signed again.
        assert signed not in self.slots_on_chain(n0, signed)
        self.assert_no_evidence()
        assert_peg_balance(main, self.nodes)


if __name__ == '__main__':
    FastBlocksRestartTest(__file__).main()
