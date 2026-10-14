#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""FastBlocks when the network splits, when a leader signs twice, and for a node that joins late.

Three validators (one key per node) and an observer, against one mainchain node:

  - equivocation: a leader signs a second block for a slot it made a block of; the nodes find the
    proof in the headers, relay it, and agree on one chain;
  - a partition: each side makes blocks of stake; each side's merged miner has the mainchain commit
    to a ticket of its side, the other side's first; when the network joins again, the side whose
    anchor the mainchain committed to first wins, whatever its length, and the other side's blocks,
    which its own anchor covered, are undone (which is why its node showed them as soft: it skipped
    a commitment);
  - an anchor of a branch the validators left: no leader can acknowledge it; the chain goes on, and
    its blocks are anchored once an anchor more than the commitments skipped covers them;
  - initial block download: the observer takes in the whole chain, blocks of stake, acknowledgements,
    BMM blocks and the branches, and comes to the same state;
  - after every step, the coins of every node are the mainchain's escrow (pegbalance).

On mock time (every node and the mainchain), moved on a slot at a time once the slot's leaders made
their blocks (SlotClock): the same slots whatever the load of the machine. (On the clock, a busy
machine let max_unanchored_slots pass between two anchors, and blocks of stake stopped.)
"""

import os
import time
from decimal import Decimal

from feature_sidechain import ACTIVATION_PERIOD, SidechainTest
from test_framework.fastblocks import KEYS, SLOT, XONLY, PosBlock, SlotClock, assert_peg_balance, stop_bmm
from test_framework.messages import CTxIn
from test_framework.script import CScript
from test_framework.util import assert_equal, assert_greater_than, rpc_port


class FastBlocksPartitionTest(SidechainTest):
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
        self.wait_until(lambda: len({n.getbestblockhash() for n in nodes}) == 1, timeout=120)

    def check_peg(self, nodes=None):
        balance = assert_peg_balance(self.main, nodes or self.validators())
        self.log.debug(f"pegbalance {balance}")


    def anchor(self, nodes):
        """One more anchor on `nodes`: wait for a merged miner's ticket request, mine it, wait for its acknowledgement."""
        before = [n.getposinfo()["anchors"] for n in nodes]
        self.wait_until(lambda: len(self.main.getrawmempool()) > 0, timeout=120)
        self.mine_main()
        # Acknowledged by the next leader that sees the commitment.
        self.clock.advance_until(lambda: all(n.getposinfo()["anchors"] > b for n, b in zip(nodes, before)), 40)
        return self.main.getblockcount()

    def validators_back(self):
        """Anchors until every validator but the banned one has been in the set three epochs on end:
        those that led slots of the other side's chain during the partition are jailed there a while,
        and in a set again once an anchor after their jail made one."""
        n0 = self.nodes[0]
        payee = n0.getnewaddress()
        n0.setbmm(True, payee, True, Decimal("0.0001"))
        since = None
        for _ in range(60):
            keys = [n.getstakinginfo()["keys"][0] for n in self.validators()]
            if all(k["inset"] or k.get("bond", {}).get("banned", False) for k in keys):
                since = self.clock.slot() if since is None else since
                if self.clock.slot() - since >= 60:
                    break
            else:
                since = None
            self.clock.advance(5)
            if self.main.getrawmempool():
                self.mine_main()
        else:
            raise AssertionError("validators not back in the set")
        self.stop_bmm(n0)
        self.wait_in_sync()

    def bid_on_own_side(self, i, other):
        """Node i's merged miner on, its ticket naming a block of its own side: not one `other` has too
        (both sides would acknowledge that one). Its merged miner names the block before its tip,
        unless that is below its last anchor point: a few more slots of its own, if needed."""
        node = self.nodes[i]
        payee = node.getaddressinfo(self.ticket_addresses[i])["scriptPubKey"]
        for _ in range(10):
            node.setbmm(True, self.ticket_addresses[i], True, Decimal("0.0001"))
            self.wait_until(lambda: any(t["payee"] == payee and "committedheight" not in t for t in node.getanchorinfo()["tickets"]), timeout=120)
            self.wait_until(lambda: len(self.main.getrawmempool()) > 0, timeout=120)
            ticket = next(t for t in node.getanchorinfo()["tickets"] if t["payee"] == payee and "committedheight" not in t)
            try:
                other.getblockheader(ticket["anchored"])
            except Exception:
                return
            self.stop_bmm(node)
            self.clock.advance(4)
        raise AssertionError(f"node {i}'s tickets name blocks of both sides")

    def stop_bmm(self, node):
        stop_bmm(self.main, self.main_address, node, self.nodes)

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
        # Mock time from here, a slot at a time: the mainchain's clock first (mining many blocks put its
        # times ahead).
        self.clock = SlotClock(self, self.nodes)
        start = max(int(time.time()), main.getblock(main.getbestblockhash())["time"]) + 5
        for i, key in enumerate(KEYS):
            path = os.path.join(self.options.tmpdir, f"validator{i}.key")
            self.restart_node(i, extra_args=self.side_args + [f"-validatorkeyfile={path}", f"-mocktime={start}"])
        self.connect_validators()
        self.clock.set_time(start)
        self.clock.advance_until(lambda: all(n.getstakinginfo()["produced"] > 0 for n in self.validators()), 60)
        self.wait_in_sync()
        # Coins for everyone, and anchors.
        for n in (n1, n2):
            txid = n0.sendtoaddress(n.getnewaddress(), 10)
            self.clock.advance_until(lambda: n0.gettransaction(txid)["confirmations"] > 0, 30)
        self.ticket_addresses = [n.getnewaddress() for n in self.validators()]
        n0.setbmm(True, self.ticket_addresses[0], True, Decimal("0.0001"))
        self.anchor(self.validators())
        self.anchor(self.validators())
        self.wait_in_sync()
        self.check_peg()

        self.log.info("Equivocation: a leader signs a second block for a slot it made a block of")
        # Node 0's or node 1's: the one that signs twice is banned, and node 2 is alone on a side below.
        self.clock.advance_until(lambda: n0.getblock(n0.getbestblockhash()).get("proposer") in XONLY[:2], 30)
        tip = n0.getblock(n0.getbestblockhash())
        assert_equal(tip["kind"], "pos")
        original = PosBlock(n0.getblock(tip["hash"], 0))
        rival = PosBlock(n0.getblock(tip["hash"], 0))
        coinbase = rival.vtx[0]
        coinbase.vin[0] = CTxIn(coinbase.vin[0].prevout, CScript(bytes(coinbase.vin[0].scriptSig) + bytes([1, 0x42])), coinbase.vin[0].nSequence)
        rival.resign(KEYS[XONLY.index(tip["proposer"])], n0.getblockhash(0))
        assert rival.hash != original.hash
        # A valid block, on the same parent: the node takes it as a branch, and sees two headers its leader signed for one slot.
        # (Valid, and not on the best chain: "inconclusive".)
        assert_equal(n1.submitblock(rival.serialize().hex()), "inconclusive")
        self.wait_until(lambda: all(len(n.listevidence()) == 1 for n in self.validators()), timeout=120)
        listed = n2.listevidence()[0]
        assert_equal((listed["pubkey"], listed["slot"]), (tip["proposer"], tip["slot"]))
        assert_equal(sorted(listed["hashes"]), sorted([original.hash, rival.hash]))
        assert_equal(n1.getblockheader(rival.hash)["confirmations"], -1)
        self.wait_in_sync()
        self.check_peg()

        self.log.info("A partition: nodes 0 and 1 on one side, node 2 on the other")
        self.stop_bmm(n0)
        self.wait_in_sync()
        self.disconnect_nodes(0, 2)
        self.disconnect_nodes(1, 2)
        fork = n0.getbestblockhash()
        # Both sides make blocks of stake, at the slots of their leaders; a payment on each.
        txid_a = n0.sendtoaddress(n0.getnewaddress(), 1)
        txid_b = n2.sendtoaddress(n2.getnewaddress(), 1)
        self.clock.advance_until(lambda: n0.gettransaction(txid_a)["confirmations"] > 0 and n2.gettransaction(txid_b)["confirmations"] > 0, 40)
        self.clock.advance_until(lambda: n0.getbestblockhash() != n2.getbestblockhash(), 20)
        # Side B's anchor is committed first (its ticket names a block of its own side).
        self.bid_on_own_side(2, n0)
        committed_b = self.anchor([n2])
        self.stop_bmm(n2)
        assert_equal(n2.getposinfo()["lastanchormainheight"], committed_b)
        # Then side A's, which side A acknowledges (its ticket names a block of its own side too).
        self.bid_on_own_side(0, n2)
        committed_a = self.anchor([n0, n1])
        self.stop_bmm(n0)
        assert_greater_than(committed_a, committed_b)
        self.wait_in_sync([n0, n1])
        assert_equal(n0.getposinfo()["anchors"], n2.getposinfo()["anchors"])
        # Side A's anchor covers its blocks, but side A skipped an earlier commitment (B's, whose
        # ticket it never saw): that may be the anchor of a chain that leaves them out. Soft.
        acked_a = n0.getanchorinfo()["recent"][0]
        assert_equal(acked_a["mainheight"], committed_a)
        assert_equal(n0.getblock(acked_a["anchored"])["finality"]["status"], "soft")
        assert_equal(n0.gettransaction(txid_a)["finality"], "soft")
        # Side B skipped a commitment too (A's, later than its own, whose ticket it never saw): with
        # one anchor more, a chain that has it would win. Soft as well.
        acked_b = n2.getanchorinfo()["recent"][0]
        assert_equal(acked_b["mainheight"], committed_b)
        assert_equal(n2.getblock(acked_b["anchored"])["finality"]["status"], "soft")
        self.check_peg([n0, n1])
        self.check_peg([n2])

        self.log.info("The network joins again: the side whose anchor came first wins")
        tip_b = n2.getbestblockhash()
        self.connect_nodes(0, 2)
        self.connect_nodes(1, 2)
        self.wait_in_sync()
        # Side B's chain: side A's acknowledgement and its blocks after the fork are undone.
        assert_equal(n0.getblock(acked_a["block"])["confirmations"], -1)
        assert_greater_than(n0.getblock(tip_b)["confirmations"], 0)
        assert_equal(n0.getblock(fork)["confirmations"] > 0, True)
        assert_equal(n0.getposinfo()["lastanchormainheight"], committed_b)
        # Side A's payment goes back to the mempool, and into a block again.
        self.clock.advance_until(lambda: n0.gettransaction(txid_a)["confirmations"] > 0, 40)
        self.clock.advance_until(lambda: n2.gettransaction(txid_b)["confirmations"] > 0, 40)
        self.wait_in_sync()
        self.check_peg()

        self.validators_back()

        self.log.info("An anchor of a branch the validators left: nobody can acknowledge it")
        # Node 2 alone on a branch; its merged miner's ticket names a block of that branch; node 2
        # stops making blocks before it could acknowledge it, and joins the others again.
        self.disconnect_nodes(0, 2)
        self.disconnect_nodes(1, 2)
        split_slot = n2.getposinfo()["slot"]
        # Two blocks of its own on its branch, at least: the block before its tip is its own.
        self.clock.advance_until(lambda: n2.getblock(n2.getblockheader(n2.getbestblockhash())["previousblockhash"]).get("slot", -1) > split_slot + 1, 40)
        assert_equal(main.getrawmempool(), [])
        n2.setbmm(True, self.ticket_addresses[2], True, Decimal("0.0001"))
        self.wait_until(lambda: len(main.getrawmempool()) > 0, timeout=120)
        n2.setstaking(False)
        n2.setbmm(False)
        self.mine_main()
        self.stop_bmm(n2)
        abandoned = main.getblockcount()
        abandoned_ticket = [t for t in n2.getanchorinfo()["tickets"] if t.get("committedheight") == abandoned][0]
        assert_greater_than(n2.getblock(abandoned_ticket["anchored"])["slot"], split_slot)
        # The others' branch is the longer (as many anchors on both): node 2 leaves its own.
        self.clock.advance_until(lambda: n0.getblockcount() > n2.getblockcount() + 2, 40)
        assert_equal(n2.getposinfo()["lastanchormainheight"] < abandoned, True)
        self.connect_nodes(0, 2)
        self.connect_nodes(1, 2)
        n2.setstaking(True)
        self.wait_in_sync()
        # The others' chain: the block the ticket names is not on it.
        assert_equal(n2.getblockheader(abandoned_ticket["anchored"])["confirmations"], -1)
        # The chain goes on; its next anchor is acknowledged, the abandoned one never is.
        n0.setbmm(True, self.ticket_addresses[0], True, Decimal("0.0001"))
        next_anchor = self.anchor(self.validators())
        assert_equal(n0.getposinfo()["lastanchormainheight"], next_anchor)
        assert all(a["mainheight"] != abandoned for a in n0.getanchorinfo(20)["recent"])
        # Its blocks: one anchor covers them, but a commitment was skipped: soft until one more.
        covered = n0.getanchorinfo()["recent"][0]["anchored"]
        assert_equal(n0.getblock(covered)["finality"]["status"], "soft")
        # Each skipped commitment that node 0 cannot place (the abandoned one, unless it learned the
        # block of its ticket) counts against every fork: an anchor more for each, at most.
        for _ in range(len(n0.getanchorinfo()["skipped"]) + 1):
            self.anchor(self.validators())
            self.wait_in_sync()
            if n0.getblock(covered)["finality"]["status"] == "final":
                break
        assert n0.getblock(covered)["finality"]["status"] == "final", (n0.getanchorinfo(20), n0.getblockheader(covered)["height"])
        self.stop_bmm(n0)
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
        assert_equal(observer.getposinfo()["anchors"], n0.getposinfo()["anchors"])
        self.check_peg(self.nodes)


if __name__ == '__main__':
    FastBlocksPartitionTest(__file__).main()
