#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""FastBlocks: a partition branch many times a peer's budget of headers the node cannot check yet
(doc/fastblocks.md, "Headers and blocks the node cannot check yet": the long branch, and its limit).

Two genesis validators, one a side, and an observer with small budgets (-posuncheckedheaders,
-posuncheckedbytes, -posuncheckedinbound), on mock time moved a slot at a time (SlotClock):

  - windows: the network splits; validator 1 and the observer on side A, which makes blocks of stake
    and no anchor; validator 0 alone on side B, whose merged miner has an anchor in every few
    mainchain blocks. The observer can check side B's first epochs (their sets are those of anchors
    below the fork), not the epochs after, whose sets side B's anchors made: several budgets of
    headers. Inbound attackers send the observer side B's first headers and siblings it cannot check
    (each holds its whole budget, the rest refused, none punished). The observer connects to
    validator 0: side B's first anchor makes its branch lead, so the observer connects it, can check
    the next epochs, and its budget frees as it connects them; it asks for the headers again and
    takes the next window, to side B's tip, the attackers connected throughout;
  - the limit, and the operator's way past it: the network splits again; side A anchors first, several
    times, then side B, more. Side B's branch leads only past its first epochs, further than the
    observer's budgets reach: the observer stays on side A (it cannot tell such a branch from an
    attacker's before it leads). The operator invalidates side A's first block after the fork: the
    observer takes side B's branch as one that catches up does, then reconsiders the block.
"""

import os
from decimal import Decimal

from feature_sidechain import SidechainTest
from test_framework.fastblocks import (
    KEYS,
    SLOT,
    XONLY,
    MsgRawBlock,
    SlotClock,
    activate_and_fund,
    assert_peg_balance,
    headers_message,
    pos_header,
    slot_of,
    stop_bmm,
    use_raw_messages,
)
from test_framework.messages import MSG_BLOCK, MSG_TYPE_MASK
from test_framework.p2p import P2PInterface
from test_framework.util import assert_equal, assert_greater_than, assert_greater_than_or_equal, rpc_port

#: The observer's budget of headers it cannot check yet, for an outbound and an inbound peer.
BUDGET = 8
#: ... and of the bytes of their blocks (these blocks of stake weigh about 300 bytes).
BYTES = 2000
ATTACKERS = 3
EPOCH = 20
#: Slots of the first partition; a mainchain block every MAIN_EVERY slots.
PARTITION_SLOTS = 300
MAIN_EVERY = 5
#: Anchors of side A after the second split, before side B's.
#: (Side B makes no anchor meanwhile, and its blocks of stake stop after max_unanchored_slots: fewer slots.)
SIDE_A_ANCHORS = 14


class SiblingsPeer(P2PInterface):
    """An attacker: sends headers, and serves the blocks it is given once told to."""

    def __init__(self, blocks):
        super().__init__()
        self.blocks = blocks
        self.serving = False
        self.pending = []

    def on_fbticket(self, message):
        pass

    def on_fbgettickets(self, message):
        pass

    def on_fbevidence(self, message):
        pass

    def on_getdata(self, message):
        if not self.serving:
            self.pending.append(message)
            return
        for inv in message.inv:
            if inv.type & MSG_TYPE_MASK != MSG_BLOCK:
                continue
            raw = self.blocks.get(f"{inv.hash:064x}")
            if raw is not None:
                self.send_without_ping(MsgRawBlock(raw))

    def serve(self):
        self.serving = True
        for message in self.pending:
            self.on_getdata(message)
        self.pending = []


class FastBlocksWindowTest(SidechainTest):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        self.main_rpc_port = rpc_port(self.num_nodes)
        self.side_args = [
            f"-sidechainslot={SLOT}",
            f"-mainchainrpcport={self.main_rpc_port}",
            "-mainchainrpcuser=main",
            "-mainchainrpcpassword=secret",
            "-mainchainrpcwallet=miner",
            f"-posvalidators={XONLY[0]},{XONLY[1]}",
            # (A block is final only after many anchors: a node takes headers it cannot check on a branch
            # from below its final block in no case, bad-pos-fork-too-deep. Side A's anchors after the
            # second split would make its blocks final, and side A's validator would never follow side B.)
            "-posfinalanchors=100",
        ]
        self.observer_args = self.side_args + [
            f"-posuncheckedheaders={BUDGET}",
            f"-posuncheckedinbound={BUDGET}",
            f"-posuncheckedbytes={BYTES}",
            "-debug=validation",
        ]
        self.extra_args = [self.side_args, self.side_args, self.observer_args]
        self.main_process = None
        self.rpc_timeout = 120

    def setup_network(self):
        self.start_mainchain()
        self.setup_nodes()
        self.connect_all()

    def connect_all(self):
        for a, b in ((0, 1), (1, 2), (0, 2)):
            self.connect_nodes(a, b)

    def anchor(self, side):
        """A mainchain block with the merged miner's ticket in it; the next leader of `side` acknowledges it."""
        before = [n.getposinfo()["anchors"] for n in side]
        self.wait_until(lambda: len(self.main.getrawmempool()) > 0, timeout=120)
        # (The clock stays: a slot passed without its leader's block would be charged to it.)
        self.mine_main()
        self.clock.advance_until(lambda: all(n.getposinfo()["anchors"] > b for n, b in zip(side, before)), 40)

    def run_slots(self, count, side):
        """`count` slots, an anchor of `side` every MAIN_EVERY of them (its merged miner on)."""
        for k in range(count):
            self.clock.step()
            if k % MAIN_EVERY == MAIN_EVERY - 1:
                self.anchor(side)

    def both_in_set(self):
        """Slots, with anchors, until both validators have been in the set for three epochs on end. One
        that led slots it could not make blocks for (on the other side of a split) is jailed a while, at
        the anchors that count those epochs, and in a set again once an anchor after its jail made one;
        after three epochs in the set, with both making their blocks, no such count is left to come."""
        validators = self.nodes[:2]
        since = None
        for _ in range(80):
            if all(n.getstakinginfo()["keys"][0]["inset"] for n in validators):
                since = self.clock.slot() if since is None else since
                if self.clock.slot() - since >= 3 * EPOCH:
                    return
            else:
                since = None
            self.run_slots(MAIN_EVERY, self.nodes)
        raise AssertionError("the validators were not in the set for three epochs on end")

    def branch_after(self, node, fork_height):
        return [node.getblock(node.getblockhash(h)) for h in range(fork_height + 1, node.getblockcount() + 1)]

    def unchecked_from(self, branch):
        """The first slot of the epochs whose sets anchors of `branch` make (the observer cannot check
        them before it connects those): two epochs after its first anchor's slot, the later of its own
        and that of the median time of the mainchain block that committed to it."""
        first = next(b for b in branch if b["kind"] == "bmm" or "ack" in b)
        anchor_slot = first.get("slot", -1)
        if "ack" in first:
            committed = self.main.getblock(self.main.getblockhash(first["ack"]["mainheight"]))
            anchor_slot = max(anchor_slot, slot_of(committed["mediantime"]))
        return (anchor_slot // EPOCH + 2) * EPOCH

    def run_test(self):
        try:
            self.run_sidechain_test()
        finally:
            self.stop_mainchain()

    def run_sidechain_test(self):
        use_raw_messages()
        main = self.main
        n0, n1, observer = self.nodes
        self.main_address = main.getnewaddress()
        self.addresses = [n0.getnewaddress(), n1.getnewaddress()]

        self.log.info("Activate slot 5, a deposit; two validators start, on mock time")
        activate_and_fund(self, funded=0)
        key_files = []
        for i in range(2):
            path = os.path.join(self.options.tmpdir, f"validator{i}.key")
            with open(path, "w") as f:
                f.write(KEYS[i].hex() + "\n")
            key_files.append(path)
        # Mock time from here: the mainchain's clock first (mining many blocks put its times ahead).
        self.clock = SlotClock(self, self.nodes)
        start = main.getblock(main.getbestblockhash())["time"] + 5
        for i in range(2):
            self.restart_node(i, extra_args=self.side_args + [f"-validatorkeyfile={key_files[i]}", f"-mocktime={start}"])
        self.restart_node(2, extra_args=self.observer_args + [f"-mocktime={start}"])
        self.connect_all()
        self.clock.set_time(start)
        n0.setbmm(True, self.addresses[0], True, Decimal("0.0001"))
        for _ in range(2):
            self.run_slots(MAIN_EVERY, self.nodes)
        self.both_in_set()
        self.wait_until(lambda: len({n.getbestblockhash() for n in self.nodes}) == 1, timeout=120)
        assert_peg_balance(main, self.nodes)

        self.log.info("Windows: the network splits; side A (validator 1 and the observer) without anchors, side B (validator 0) with")
        # (No ticket of the chain before the split waits: one that names a block both sides have, both acknowledge.)
        stop_bmm(main, self.main_address, n0, self.nodes)
        self.disconnect_nodes(0, 1)
        self.disconnect_nodes(0, 2)
        fork = n0.getbestblockhash()
        fork_height = n0.getblockcount()
        anchors_a = n1.getposinfo()["anchors"]
        # Side B's tickets name blocks of its own.
        self.clock.advance_until(lambda: n0.getblockcount() > fork_height + 2, 20)
        n0.setbmm(True, self.addresses[0], True, Decimal("0.0001"))
        self.run_slots(PARTITION_SLOTS, [n0])
        self.wait_until(lambda: observer.getbestblockhash() == n1.getbestblockhash(), timeout=120)
        assert_greater_than(observer.getblockcount(), fork_height)
        assert_equal(observer.getposinfo()["anchors"], anchors_a)
        assert_greater_than(n0.getposinfo()["anchors"], anchors_a + 5)
        assert observer.getblockheader(fork)["confirmations"] > 0
        branch = self.branch_after(n0, fork_height)
        unchecked_from = self.unchecked_from(branch)
        unchecked = [b for b in branch if b.get("slot", -1) >= unchecked_from]
        self.log.info(f"Side B: {len(branch)} blocks after the fork, {len(unchecked)} the observer cannot check yet (budget {BUDGET})")
        assert_greater_than(len(unchecked), 4 * BUDGET)

        self.log.info("Inbound attackers send side B's first headers and siblings the observer cannot check")
        # The headers below the attackers' siblings: those the observer can check.
        boundary = unchecked_from
        prefix = []
        for b in branch:
            if b.get("slot", -1) >= boundary:
                break
            prefix.append(b)
        # What the honest peer alone gives the observer: several budgets.
        assert_greater_than(len(branch) - len(prefix), 3 * BUDGET)
        prefix_headers = [bytes.fromhex(n0.getblockheader(b["hash"], False)) for b in prefix]
        parent = prefix[-1]["hash"]
        blocks = {b["hash"]: bytes.fromhex(n0.getblock(b["hash"], 0)) for b in prefix}
        base = len(observer.getchaintips())
        attackers = [observer.add_p2p_connection(SiblingsPeer(blocks)) for _ in range(ATTACKERS)]
        with observer.assert_debug_log(["unchecked-budget"]):
            for k, attacker in enumerate(attackers):
                attacker.send_without_ping(headers_message(prefix_headers))
                for s in range(BUDGET + 10):
                    sibling = pos_header(parent, boundary + 1, merkle=(1000 * (k + 1) + s).to_bytes(32, "little"))
                    attacker.send_without_ping(headers_message([sibling]))
                attacker.sync_with_ping(timeout=60)
        held = len(observer.getchaintips()) - base
        self.log.info(f"{held} new chain tips with {ATTACKERS} attackers connected")
        assert_greater_than(ATTACKERS * BUDGET + 2, held)
        assert_greater_than_or_equal(held, ATTACKERS * (BUDGET - 1))
        assert all(a.is_connected for a in attackers)
        # They serve the blocks of side B's first headers (the observer asked them, the only peers that had them).
        for attacker in attackers:
            attacker.serve()

        self.log.info("The observer connects to validator 0: side B's branch, in windows of its budget")
        self.connect_nodes(2, 0)
        n0_id = next(p["id"] for p in observer.getpeerinfo() if p["connection_type"] == "manual")
        self.wait_until(lambda: observer.getbestblockhash() == n0.getbestblockhash(), timeout=300)
        # The attackers connected throughout, each within its budget; nobody punished.
        assert all(a.is_connected for a in attackers)
        with open(observer.debug_log_path, encoding="utf-8") as log:
            text = log.read()
        assert "Misbehaving" not in text
        # The honest peer's headers were refused for its budget on the way (windows), and taken later.
        over = self.refused(observer, n0_id)
        self.log.info(f"{over} headers of the honest peer refused for its budget, taken again later")
        assert_greater_than_or_equal(over, 1)
        for attacker in attackers:
            attacker.peer_disconnect()
            attacker.wait_for_disconnect()
        self.connect_nodes(1, 0)
        self.wait_until(lambda: len({n.getbestblockhash() for n in self.nodes}) == 1, timeout=120)
        # (Validator 1 led slots of side B's chain it could not make blocks for: jailed there.)
        self.both_in_set()
        stop_bmm(main, self.main_address, n0, self.nodes)
        assert_peg_balance(main, self.nodes)

        self.log.info("The limit: the network splits again; side A anchors first, then side B, more")
        self.disconnect_nodes(0, 1)
        self.disconnect_nodes(0, 2)
        fork = n0.getbestblockhash()
        fork_height = n0.getblockcount()
        self.clock.advance_until(lambda: n1.getblockcount() > fork_height + 2 and n0.getblockcount() > fork_height + 2, 20)
        n1.setbmm(True, self.addresses[1], True, Decimal("0.0001"))
        for _ in range(SIDE_A_ANCHORS):
            self.run_slots(MAIN_EVERY, [n1, observer])
        stop_bmm(main, self.main_address, n1, self.nodes)
        n0.setbmm(True, self.addresses[0], True, Decimal("0.0001"))
        anchors_a = observer.getposinfo()["anchors"]
        while n0.getposinfo()["anchors"] <= anchors_a + 3:
            self.run_slots(MAIN_EVERY, [n0])
        branch = self.branch_after(n0, fork_height)
        # Where side B's branch leads: its first block with more anchors than side A's chain.
        leads = next(i for i, b in enumerate(branch) if n0.getblockheader(b["hash"])["chainwork"] > observer.getblockheader(observer.getbestblockhash())["chainwork"])
        unchecked_from = self.unchecked_from(branch)
        before_lead = [b for b in branch[:leads + 1] if b.get("slot", -1) >= unchecked_from]
        self.log.info(f"Side B leads at its block {leads + 1} after the fork; {len(before_lead)} before it the observer cannot check (budget {BUDGET}, {BYTES} bytes)")
        assert_greater_than(len(before_lead), BUDGET)

        self.log.info("The observer does not follow side B's branch: it cannot tell it from an attacker's before it leads")
        refused_before = self.refused(observer, None)
        self.connect_nodes(2, 0)
        n0_id = next(p["id"] for p in observer.getpeerinfo() if p["connection_type"] == "manual")
        # Its headers past the budget refused; the clock does not move, nor does the observer's tip:
        # nothing more comes.
        self.wait_until(lambda: self.refused(observer, n0_id) > 0 and self.refused(observer, None) > refused_before, timeout=120)
        self.wait_until(lambda: any(t["status"] == "headers-only" for t in observer.getchaintips()), timeout=60)
        observer.syncwithvalidationinterfacequeue()
        assert observer.getblockheader(fork)["confirmations"] > 0
        assert observer.getbestblockhash() != n0.getbestblockhash()

        self.log.info("The operator invalidates side A's first block after the fork; the observer takes side B's branch")
        first_a = observer.getblockhash(fork_height + 1)
        observer.invalidateblock(first_a)
        self.wait_until(lambda: observer.getbestblockhash() == n0.getbestblockhash(), timeout=300)
        observer.reconsiderblock(first_a)
        assert_equal(observer.getbestblockhash(), n0.getbestblockhash())
        assert_equal(observer.getblockheader(first_a)["confirmations"], -1)
        with open(observer.debug_log_path, encoding="utf-8") as log:
            assert "Misbehaving" not in log.read()
        # Side A's validator, whatever its budgets: when side A made more anchors since the fork than the
        # window of sets its state keeps (evidence_window_anchors: whether it did depends on where the
        # sets of side B's first epochs were made), it takes no header it cannot check on side B's branch
        # (bad-pos-fork-too-deep), which the same operator's step gets it past.
        self.connect_nodes(1, 0)
        self.wait_until(lambda: n1.getbestblockhash() == n0.getbestblockhash() or self.too_deep(n1) > 0, timeout=120)
        if n1.getbestblockhash() != n0.getbestblockhash():
            self.log.info("Side A's validator: past the window of sets, the operator's step too")
            n1.invalidateblock(first_a)
            self.wait_until(lambda: n1.getbestblockhash() == n0.getbestblockhash(), timeout=300)
            n1.reconsiderblock(first_a)
        self.wait_until(lambda: len({n.getbestblockhash() for n in self.nodes}) == 1, timeout=120)
        stop_bmm(main, self.main_address, n0, self.nodes)
        assert_peg_balance(main, self.nodes)

    @staticmethod
    def too_deep(node):
        with open(node.debug_log_path, encoding="utf-8") as log:
            return sum(1 for line in log if "bad-pos-fork-too-deep" in line)

    @staticmethod
    def refused(node, peer_id):
        """Headers from the peer (any, if None) refused for its budget, as the node's log says."""
        sender = "" if peer_id is None else f"from peer={peer_id} "
        with open(node.debug_log_path, encoding="utf-8") as log:
            return sum(1 for line in log if f"{sender}over the budget (unchecked-budget)" in line)


if __name__ == '__main__':
    FastBlocksWindowTest(__file__).main()
