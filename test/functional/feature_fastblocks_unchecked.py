#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""FastBlocks: headers and blocks a node cannot check yet, against attackers (doc/fastblocks.md,
"Headers and blocks the node cannot check yet"). Three bonded validators (no -posvalidators: a node
that catches up cannot tell the leaders of the epochs ahead of its blocks) and a node that joins late,
with peers of the test framework:

  - reconnections: an attacker sends the honest headers with forged signatures and thousands of
    unsigned siblings, leaves, and comes back, again and again: the block index never holds more
    than a peer's budget of such headers, and what the attacker held goes with it (it grew by a
    budget each time: finding 6a of the review of audit 6);
  - many attackers at once: each holds its own budget and no more, so the node holds no more than
    their budgets (a node-wide total, which attackers sharing headers locked, is gone);
  - an attacker serves forged copies of the honest blocks and inflates the best header with headers
    nobody can check, then honest nodes connect: the node catches up to the honest tip, every
    signature it keeps is the leader's, no honest peer is punished, and when the attacker leaves its
    headers go (findings 2, 7 and 8);
  - inbound attackers, more than may hold such headers at once, against a node that joins last with a
    small inbound budget (-posuncheckedinbound): none holds more than that budget, no more than 8 hold
    any, the others are refused and not punished; then the node syncs from honest outbound peers a
    stretch of such headers several times the inbound budget, the attackers still connected (review
    of audit 6, item 10).
"""

import time
from decimal import Decimal

from feature_sidechain import SidechainTest
from test_framework.fastblocks import (
    POS_HEADER_SIZE,
    SLOT,
    MsgRawBlock,
    activate_and_fund,
    assert_peg_balance,
    bond_validators,
    chain_headers,
    header_hash,
    headers_message,
    pos_header,
    use_raw_messages,
    write_key_file,
)
from test_framework.messages import MSG_BLOCK, MSG_TYPE_MASK
from test_framework.p2p import P2PInterface
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error, rpc_port

#: Headers a node takes in one message, at most.
MAX_HEADERS = 2000
#: The budget of headers the node cannot check yet, regtest's: each peer's, whatever the others hold.
UNCHECKED_PER_PEER = 2000
SIBLINGS = 2500
ROUNDS = 4
ATTACKERS = 4
#: The last node's budget of such headers for an inbound peer (-posuncheckedinbound), and how many
#: inbound peers may hold any at once (regtest's, as main's).
INBOUND_BUDGET = 25
INBOUND_HOLDERS = 8


class FastBlocksPeer(P2PInterface):
    """A peer that reads FastBlocks' messages and ignores them."""

    def on_fbticket(self, message):
        pass

    def on_fbgettickets(self, message):
        pass

    def on_fbevidence(self, message):
        pass


class ForgingPeer(FastBlocksPeer):
    """A peer that answers every request for a block with a copy whose leader's signature is forged
    (the block hash does not cover it)."""

    def __init__(self, blocks):
        super().__init__()
        self.blocks = blocks
        self.served = 0

    def on_getdata(self, message):
        for inv in message.inv:
            if inv.type & MSG_TYPE_MASK != MSG_BLOCK:
                continue
            raw = self.blocks.get(f"{inv.hash:064x}")
            if raw is None:
                continue
            self.send_without_ping(MsgRawBlock(raw))
            self.served += 1


class ServingPeer(ForgingPeer):
    """A peer that answers requests for the blocks it is given (honest ones), once told to serve."""

    def __init__(self, blocks):
        super().__init__(blocks)
        self.serving = False
        self.pending = []

    def on_getdata(self, message):
        if self.serving:
            super().on_getdata(message)
        else:
            self.pending.append(message)

    def serve(self):
        self.serving = True
        for message in self.pending:
            super().on_getdata(message)
        self.pending = []


class FastBlocksUncheckedTest(SidechainTest):
    def set_test_params(self):
        self.num_nodes = 5
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
        self.extra_args = [self.side_args] * 4 + [self.side_args + [f"-posuncheckedinbound={INBOUND_BUDGET}"]]
        self.main_process = None

    def setup_network(self):
        # Nodes 3 and 4 join late.
        self.start_mainchain()
        self.setup_nodes()
        self.connect_validators()

    def connect_validators(self):
        for a, b in ((0, 1), (1, 2), (0, 2)):
            self.connect_nodes(a, b)

    def validators(self):
        return self.nodes[:3]

    def wait_in_sync(self, nodes=None, timeout=90):
        nodes = nodes or self.validators()
        self.wait_until(lambda: len({n.getbestblockhash() for n in nodes}) == 1, timeout=timeout)

    def anchor(self):
        """One more anchor: mine the merged miner's ticket requests until one is acknowledged."""
        before = [n.getposinfo()["anchors"] for n in self.validators()]

        def anchored():
            if all(n.getposinfo()["anchors"] > b for n, b in zip(self.validators(), before)):
                return True
            if self.main.getrawmempool():
                self.mine_main()
            time.sleep(1)
            return False
        self.wait_until(anchored, timeout=120)

    def validator_args(self, i):
        return self.side_args + [f"-validatorkeyfile={self.key_files[i]}", f"-validatorpayout={self.payouts[i]}"]

    @staticmethod
    def tips(node):
        return len(node.getchaintips())

    def send_headers(self, peer, headers):
        for start in range(0, len(headers), MAX_HEADERS):
            peer.send_without_ping(headers_message(headers[start:start + MAX_HEADERS]))

    def send_siblings(self, peer, parent, slot, tag, count=SIBLINGS):
        for k in range(count):
            peer.send_without_ping(headers_message([pos_header(parent, slot, merkle=(tag * 100000 + k).to_bytes(32, "little"))]))

    def run_test(self):
        try:
            self.run_sidechain_test()
        finally:
            self.stop_mainchain()

    def run_sidechain_test(self):
        use_raw_messages()
        main = self.main
        n0, n1, n2, late = self.nodes[:4]
        self.main_address = main.getnewaddress()

        self.log.info("Activate slot 5, a deposit; three validators bond, blocks of stake start; anchors")
        activate_and_fund(self, funded=0, nodes=self.validators())
        self.key_files = [write_key_file(self, i) for i in range(3)]
        self.payouts = [n.getnewaddress() for n in self.validators()]
        for i in range(3):
            self.restart_node(i, extra_args=self.validator_args(i))
        self.connect_validators()
        bond_validators(self, n0, range(3), nodes=self.validators())
        self.wait_until(lambda: all(n.getstakinginfo()["produced"] > 0 for n in self.validators()), timeout=180)
        self.wait_in_sync()
        n0.setbmm(True, n0.getnewaddress(), True, Decimal("0.0001"))
        for _ in range(4):
            self.anchor()
        # A few epochs of blocks of stake.
        start = n0.getblockcount()
        self.wait_until(lambda: n0.getblockcount() >= start + 50, timeout=120)
        self.anchor()
        self.wait_in_sync()
        assert_peg_balance(main, self.validators())

        honest = chain_headers(n0)
        forged = [bytearray(h) for h in honest]
        pos = [i for i, h in enumerate(forged) if len(h) == POS_HEADER_SIZE]
        assert_greater_than(len(pos), 40)
        for i in pos:
            forged[i][112:176] = bytes(64)
        newest = header_hash(forged[-1])
        slot = n0.getblock(newest)["slot"]
        self.wait_until(lambda: n0.getposinfo()["slot"] > slot + 3, timeout=60)
        base = self.tips(late)

        self.log.info("Reconnections: an attacker sends headers nobody can check, leaves, and comes back")
        for r in range(ROUNDS):
            attacker = late.add_p2p_connection(FastBlocksPeer())
            self.send_headers(attacker, forged)
            self.send_siblings(attacker, newest, slot + 1, r + 1)
            attacker.sync_with_ping(timeout=120)
            assert attacker.is_connected
            held = self.tips(late)
            self.log.debug(f"round {r}: {held} chain tips with the attacker connected")
            # The forged chain and the siblings: the peer's budget, not more.
            assert_greater_than(UNCHECKED_PER_PEER + base + 2, held)
            attacker.peer_disconnect()
            attacker.wait_for_disconnect()
            # What it alone held goes with it.
            self.wait_until(lambda: self.tips(late) <= base + 1, timeout=30)
        assert_equal(late.getblockcount(), 0)

        self.log.info("Many attackers at once: each holds its own budget, no more")
        attackers = [late.add_p2p_connection(FastBlocksPeer()) for _ in range(ATTACKERS)]
        for k, attacker in enumerate(attackers):
            self.send_headers(attacker, forged)
            self.send_siblings(attacker, newest, slot + 1, 10 + k, count=1500)
        for attacker in attackers:
            attacker.sync_with_ping(timeout=600)
            assert attacker.is_connected
        held = self.tips(late)
        self.log.debug(f"{held} chain tips with {ATTACKERS} attackers connected")
        assert_greater_than(ATTACKERS * UNCHECKED_PER_PEER + base + 2, held)
        for attacker in attackers:
            attacker.peer_disconnect()
            attacker.wait_for_disconnect()
        self.wait_until(lambda: self.tips(late) <= base + 1, timeout=30)

        self.log.info("An attacker serves forged copies of the blocks and inflates the best header; honest nodes connect")
        blocks = {}
        for h in range(1, len(honest) + 1):
            block_hash = n0.getblockhash(h)
            raw = bytearray.fromhex(n0.getblock(block_hash, 0))
            if len(honest[h - 1]) == POS_HEADER_SIZE:
                raw[112:176] = bytes(64)
            blocks[block_hash] = bytes(raw)
        attacker = late.add_p2p_connection(ForgingPeer(blocks))
        self.send_headers(attacker, forged)
        # Headers nobody can check that lead further than anything (one a slot after the honest tip).
        padded = []
        prev = newest
        for k in range(60):
            padded.append(pos_header(prev, slot + 1 + k, merkle=b"\x09" * 32))
            prev = header_hash(padded[-1])
        self.wait_until(lambda: n0.getposinfo()["slot"] > slot + 62, timeout=120)
        self.send_headers(attacker, padded)
        attacker.sync_with_ping(timeout=60)
        # The node asks the attacker for the blocks; the first forged one fails as it is connected (the
        # copy's fault: the chain stays, no fault of anyone's).
        self.wait_until(lambda: attacker.served > 0, timeout=60)

        for i in range(3):
            self.connect_nodes(3, i)
        honest_ids = {p["id"] for p in late.getpeerinfo() if not p["inbound"]}
        assert_equal(len(honest_ids), 3)
        # The node reaches the honest tip, the attacker still connected (its forged copies fail as
        # the node connects them, which is the copy's fault: the honest peers' headers give the
        # leaders' signatures).
        self.wait_in_sync(self.nodes[:4], timeout=240)
        assert_greater_than(attacker.served, 0)
        for i in pos[:5] + pos[-5:]:
            assert_equal(late.getblockheader(header_hash(forged[i]), False), honest[i].hex())
        assert_equal(late.getblock(header_hash(forged[pos[0]]), 0), n0.getblock(header_hash(forged[pos[0]]), 0))
        # No honest peer was punished: all three still connected, none discouraged.
        assert_equal({p["id"] for p in late.getpeerinfo() if not p["inbound"]}, honest_ids)
        with open(late.debug_log_path, encoding="utf-8") as log:
            text = log.read()
        for peer_id in honest_ids:
            assert f"Misbehaving: peer={peer_id}" not in text, f"peer {peer_id} punished"
        # What the attacker alone held (its padded headers) goes with it.
        attacker.peer_disconnect()
        attacker.wait_for_disconnect()
        for header in padded:
            assert_raises_rpc_error(-5, "Block not found", late.getblockheader, header_hash(header))

        self.inbound_attackers()

        self.log.info("Anchors again; every node agrees")
        self.anchor()
        self.wait_in_sync(self.nodes)
        assert_peg_balance(main, self.nodes)

    def inbound_attackers(self):
        self.log.info("Inbound attackers, more than may hold headers nobody can check; then honest outbound peers")
        n0, last = self.nodes[0], self.nodes[4]
        assert_equal(last.getblockcount(), 0)
        honest = chain_headers(n0)
        pos = [i for i, h in enumerate(honest) if len(h) == POS_HEADER_SIZE]
        # The checked headers below the first block of stake, which every attacker sends, and siblings
        # of that block of its own, which the node cannot check (the attackers serve the blocks below
        # only once all have sent theirs: connected, the anchors below would let the node check the
        # siblings, and refuse them as unsigned).
        first = pos[0]
        prefix = honest[:first]
        parent = header_hash(prefix[-1])
        slot = n0.getblock(header_hash(honest[first]))["slot"]
        # The stretch the node cannot check, which honest peers give it: several inbound budgets.
        assert_greater_than(len(honest) - first, 3 * INBOUND_BUDGET)
        blocks = {n0.getblockhash(h): bytes.fromhex(n0.getblock(n0.getblockhash(h), 0)) for h in range(1, first + 1)}
        base = self.tips(last)
        attackers = [last.add_p2p_connection(ServingPeer(blocks)) for _ in range(INBOUND_HOLDERS + 3)]
        with last.assert_debug_log(["unchecked-budget"]):
            for k, attacker in enumerate(attackers):
                self.send_headers(attacker, prefix)
                self.send_siblings(attacker, parent, slot, 30 + k, count=INBOUND_BUDGET + 20)
                attacker.sync_with_ping(timeout=120)
        held = self.tips(last) - base
        self.log.debug(f"{held} new chain tips with {len(attackers)} inbound attackers connected")
        # Each sibling is a tip: INBOUND_HOLDERS attackers hold INBOUND_BUDGET each, the others none.
        assert_greater_than(INBOUND_HOLDERS * INBOUND_BUDGET + 2, held)
        assert_greater_than(held, (INBOUND_HOLDERS - 1) * INBOUND_BUDGET)
        connected = [a for a in attackers if a.is_connected]
        assert_equal(len(connected), len(attackers))
        # None punished for it.
        with open(last.debug_log_path, encoding="utf-8") as log:
            assert "Misbehaving" not in log.read()

        # The attackers serve the blocks below (the node asked them, the only peers that had them); honest
        # outbound peers: the node syncs the whole stretch, far over the inbound budget.
        for attacker in attackers:
            attacker.serve()
        for i in range(3):
            self.connect_nodes(4, i)
        self.wait_until(lambda: last.getbestblockhash() == n0.getbestblockhash() or last.getblockcount() >= len(honest), timeout=300)
        assert_greater_than(last.getblockcount(), len(honest) - 1)
        assert_equal(last.getblockhash(len(honest)), header_hash(honest[-1]))
        # The attackers still connected (the node asks them for their siblings' blocks, which they do
        # not serve: a block download may time out).
        still = [a for a in attackers if a.is_connected]
        self.log.debug(f"{len(still)} of {len(attackers)} attackers still connected after the sync")
        assert_greater_than(len(still), 0)
        for attacker in attackers:
            if attacker.is_connected:
                attacker.peer_disconnect()
                attacker.wait_for_disconnect()
        self.wait_until(lambda: self.tips(last) <= base + 1, timeout=30)


if __name__ == '__main__':
    FastBlocksUncheckedTest(__file__).main()
