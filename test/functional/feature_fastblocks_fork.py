#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""FastBlocks: a fork whose overtaking block a node cannot check, and has the header of with a forged
signature, still syncs.

Three validators (one key per node) and an observer, against one mainchain node. The network splits:
side A (nodes 0 and 1, and the observer) anchors first, side B (node 2) after, so side A stays ahead
(as many anchors, the earlier last commitment). Side B goes on for two epochs, so that the set of its
next epoch is that of its own anchor, which the observer never connected, then anchors again: that
block, H, makes side B overtake. A peer gives the observer side B's headers with H's signature forged
(the block hash does not cover it). Then node 2 connects: the observer cannot check the leader's
signature of H before it connects side B, nor connect side B without H. It takes H's block with the
header's signature, keeping the other one, and when it connects side B the valid signature replaces
the forged one. (It turned H away before, and asked for it again only once its parent was connected,
which never came: the download of side B stalled for good.)
"""

import os
import time
from decimal import Decimal

from feature_sidechain import ACTIVATION_PERIOD, SidechainTest
from test_framework.fastblocks import (
    KEYS,
    REGTEST_GENESIS_TIME,
    SLOT,
    XONLY,
    assert_peg_balance,
    header_hash,
    headers_message,
    stop_bmm,
    use_raw_messages,
)
from test_framework.p2p import P2PInterface
from test_framework.util import assert_equal, assert_greater_than, rpc_port

SLOTS_PER_EPOCH = 20


class FastBlocksPeer(P2PInterface):
    """A peer that reads FastBlocks' messages and ignores them."""

    def on_fbticket(self, message):
        pass

    def on_fbgettickets(self, message):
        pass

    def on_fbevidence(self, message):
        pass


class FastBlocksForkTest(SidechainTest):
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
        # The observer calls a block final only once 5 anchors cover it: side B's headers, which it
        # cannot check, leave its chain above its final block (it takes them).
        self.extra_args = [self.side_args] * 3 + [self.side_args + ["-posfinalanchors=5"]]
        self.main_process = None

    def setup_network(self):
        self.start_mainchain()
        self.setup_nodes()
        self.connect_all()

    def connect_all(self):
        for a, b in ((0, 1), (1, 2), (0, 2), (3, 0), (3, 1)):
            self.connect_nodes(a, b)

    def wait_in_sync(self, nodes, timeout=60):
        self.wait_until(lambda: len({n.getbestblockhash() for n in nodes}) == 1, timeout=timeout)

    def anchor(self, nodes):
        """One more anchor on `nodes`: wait for a merged miner's ticket request, mine it, wait for its acknowledgement."""
        before = [n.getposinfo()["anchors"] for n in nodes]
        self.wait_until(lambda: len(self.main.getrawmempool()) > 0, timeout=60)
        self.mine_main()
        self.wait_until(lambda: all(n.getposinfo()["anchors"] > b for n, b in zip(nodes, before)), timeout=60)
        return self.main.getblockcount()

    def stop_bmm(self, node):
        stop_bmm(self.main, self.main_address, node, self.nodes)

    def run_test(self):
        try:
            self.run_sidechain_test()
        finally:
            self.stop_mainchain()

    def run_sidechain_test(self):
        use_raw_messages()
        main = self.main
        n0, n1, n2, observer = self.nodes
        validators = self.nodes[:3]
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
        self.wait_until(lambda: all(n.getbestblockhash() == block["hash"] for n in self.nodes))
        for i, key in enumerate(KEYS):
            path = os.path.join(self.options.tmpdir, f"validator{i}.key")
            with open(path, "w") as f:
                f.write(key.hex() + "\n")
            self.restart_node(i, extra_args=self.side_args + [f"-validatorkeyfile={path}"])
        self.connect_all()
        self.wait_until(lambda: all(n.getstakinginfo()["produced"] > 0 for n in validators), timeout=120)
        self.addresses = [n.getnewaddress() for n in validators]
        n0.setbmm(True, self.addresses[0], True, Decimal("0.0001"))
        self.anchor(self.nodes)
        self.anchor(self.nodes)
        self.stop_bmm(n0)
        self.wait_in_sync(self.nodes)
        # The mainchain's clock is the one anchors' slots go by: not ahead of the nodes' (mining many
        # blocks at once puts it ahead).
        self.wait_until(lambda: main.getblock(main.getbestblockhash())["time"] <= time.time() + 2, timeout=600)
        assert_peg_balance(main, self.nodes)

        self.log.info("A partition: nodes 0 and 1 and the observer on side A, node 2 on side B")
        self.disconnect_nodes(0, 2)
        self.disconnect_nodes(1, 2)
        self.wait_until(lambda: n0.getbestblockhash() != n2.getbestblockhash(), timeout=30)
        # Side A anchors first, side B after: side A stays ahead (the earlier last commitment).
        n0.setbmm(True, self.addresses[0], True, Decimal("0.0001"))
        committed_a = self.anchor([n0, n1, observer])
        self.stop_bmm(n0)
        n2.setbmm(True, self.addresses[2], True, Decimal("0.0001"))
        committed_b = self.anchor([n2])
        self.stop_bmm(n2)
        assert_greater_than(committed_b, committed_a)
        assert_equal(n0.getposinfo()["anchors"], n2.getposinfo()["anchors"])
        anchor_b = n2.getanchorinfo()["recent"][0]
        assert_equal(anchor_b["mainheight"], committed_b)
        # The slot of side B's anchor: that of its block, or of the mainchain block that committed to it if later.
        anchor_slot = max(n2.getblock(anchor_b["block"])["slot"], main.getblock(main.getblockhash(committed_b))["time"] - REGTEST_GENESIS_TIME)
        # Two epochs on: the set of side B's next epoch is that of its anchor.
        boundary = (anchor_slot // SLOTS_PER_EPOCH + 2) * SLOTS_PER_EPOCH
        self.wait_until(lambda: n2.getposinfo()["slot"] >= boundary + 1, timeout=120)
        assert_equal(observer.getbestblockhash(), n0.getbestblockhash())

        self.log.info("Side B anchors again: H, the block that makes it overtake")
        n2.setbmm(True, self.addresses[2], True, Decimal("0.0001"))
        self.anchor([n2])
        self.stop_bmm(n2)
        h = n2.getanchorinfo()["recent"][0]["block"]
        assert_greater_than(n2.getblock(h)["slot"], boundary)
        assert_greater_than(n2.getposinfo()["anchors"], n0.getposinfo()["anchors"])

        self.log.info("A peer gives the observer side B's headers, H's signature forged")
        # Side B's headers from the fork on (the first block of node 2's chain the observer lacks).
        height = n2.getblockcount()
        headers = []
        while True:
            hash_ = n2.getblockhash(height)
            if observer_has(observer, hash_):
                break
            headers.append(bytearray.fromhex(n2.getblockheader(hash_, False)))
            height -= 1
        headers.reverse()
        assert_greater_than(len(headers), 2)
        index = [header_hash(x) for x in headers].index(h)
        honest_h = bytes(headers[index])
        headers[index][112:176] = bytes(64)
        attacker = observer.add_p2p_connection(FastBlocksPeer())
        attacker.send_and_ping(headers_message(headers))
        assert_equal(observer.getblockheader(header_hash(headers[-1]))["height"], n2.getblockcount())
        assert observer.getblockheader(h, False) != honest_h.hex()
        attacker.peer_disconnect()
        attacker.wait_for_disconnect()
        assert_equal(observer.getbestblockhash(), n0.getbestblockhash())

        self.log.info("Node 2 connects to the observer: it syncs side B, H with its leader's signature")
        with observer.assert_debug_log(["which cannot be checked yet: taken with its header's"], timeout=60):
            self.connect_nodes(3, 2)
            self.wait_until(lambda: observer.getblockheader(h)["confirmations"] > 0, timeout=60)
        assert_equal(observer.getblockheader(h, False), honest_h.hex())
        assert_equal(observer.getblock(h, 0), n2.getblock(h, 0))

        self.log.info("The network joins again: one chain, side B's")
        self.connect_nodes(0, 2)
        self.connect_nodes(1, 2)
        self.wait_in_sync(self.nodes, timeout=90)
        assert_peg_balance(main, self.nodes)


def observer_has(node, hash_):
    try:
        return node.getblockheader(hash_)["confirmations"] > 0
    except Exception:
        return False


if __name__ == '__main__':
    FastBlocksForkTest(__file__).main()
