#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""FastBlocks against peers that misbehave. Three bonded validators (no -posvalidators: a node that
catches up cannot tell the leaders of the epochs ahead of its blocks) and a node that joins late,
with peers of the test framework:

  - a node catching up gets the honest headers first with their signatures forged (the block hash
    does not cover them), and a block with one; then thousands of unsigned siblings of the newest
    header, and a message padded with more: the node takes no more of those it cannot check from the
    peer than its budget (unchecked_headers_per_peer); then an honest node connects: it reaches the
    honest tip;
  - a validator restarted (the sets it read forgotten, the state's pruned) gets about 70 unsigned
    headers from deep in the chain, acknowledging every commitment after the fork: refused, no new
    chain tip, no reorg; and thousands of unsigned siblings of a deep block, then of its tip;
  - tickets: a peer's tickets nobody committed to, 8 per mainchain block at most; asking for 17
    tickets at once is misbehaving;
  - a proof that a leader signed twice, sent 200 times: kept once, the peer ignored.
"""

import time
from decimal import Decimal

from feature_sidechain import SidechainTest
from test_framework.fastblocks import (
    KEYS,
    POS_HEADER_SIZE,
    SLOT,
    XONLY,
    MsgEvidence,
    MsgGetTickets,
    MsgRawBlock,
    MsgTicket,
    activate_and_fund,
    assert_peg_balance,
    block_sighash,
    bond_validators,
    chain_headers,
    header_hash,
    headers_message,
    pos_header,
    ticket_bytes,
    ticket_hash,
    use_raw_messages,
    write_key_file,
)
from test_framework.key import sign_schnorr
from test_framework.messages import ser_compact_size
from test_framework.p2p import P2PInterface
from test_framework.authproxy import JSONRPCException
from test_framework.util import assert_equal, assert_greater_than, rpc_port

#: Slots per epoch on regtest.
SLOTS_PER_EPOCH = 20


def epoch_of(slot):
    return slot // SLOTS_PER_EPOCH


#: Headers a node takes in one message, at most.
MAX_HEADERS = 2000
SIBLINGS = 5000
#: Headers whose signature it cannot check yet a node takes from a peer, until it checked them (regtest).
UNCHECKED_PER_PEER = 2000


class FastBlocksPeer(P2PInterface):
    """A peer that reads FastBlocks' messages and ignores them."""

    def __init__(self):
        super().__init__()
        self.tickets = []

    def on_fbticket(self, message):
        self.tickets.append(message.data)

    def on_fbgettickets(self, message):
        pass

    def on_fbevidence(self, message):
        pass


class FastBlocksP2PTest(SidechainTest):
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
        # Node 3 joins late.
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
        self.wait_until(lambda: len({n.getbestblockhash() for n in nodes}) == 1, timeout=90)

    def anchor(self):
        """One more anchor: mine the merged miner's ticket requests until one is acknowledged (a ticket
        made before blocks of stake halted for want of anchors is not)."""
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
        return {(t["hash"], t["status"]) for t in node.getchaintips()}

    @staticmethod
    def side_tips(node):
        """The chain tips off the active chain (the active one moves on as the test runs)."""
        return {t["hash"] for t in node.getchaintips() if t["status"] != "active"}

    @staticmethod
    def unknown(node, headers):
        """Whether the node has none of these headers."""
        for header in headers:
            try:
                node.getblockheader(header_hash(header))
                return False
            except JSONRPCException:
                pass
        return True

    def run_test(self):
        try:
            self.run_sidechain_test()
        finally:
            self.stop_mainchain()

    def run_sidechain_test(self):
        use_raw_messages()
        main = self.main
        n0, n1, n2, late = self.nodes
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
        # More anchors than the window of sets the state keeps (10 on regtest), and an epoch more: the
        # sets of the first epochs of stake are pruned (a set goes once 10 anchors came after the
        # anchor that made it, and the anchor after that one is older than the epoch before the
        # current one).
        for _ in range(12):
            self.anchor()
        self.wait_in_sync()
        acks = sorted(n0.getblock(a["block"])["slot"] for a in n0.getanchorinfo(30)["recent"] if n0.getblock(a["block"])["kind"] == "pos")
        self.wait_until(lambda: n0.getposinfo()["epoch"] >= epoch_of(acks[0]) + 2, timeout=60)
        self.anchor()
        self.wait_in_sync()
        assert_peg_balance(main, self.validators())

        self.log.info("A node catching up gets the honest headers with forged signatures first")
        honest = chain_headers(n0)
        forged = [bytearray(h) for h in honest]
        pos = [i for i, h in enumerate(forged) if len(h) == POS_HEADER_SIZE]
        assert_greater_than(len(pos), 20)
        for i in pos:
            forged[i][112:176] = bytes(64)
        assert_equal([header_hash(h) for h in forged[-3:]], [n0.getblockhash(n0.getblockcount() - k) for k in (2, 1, 0)])
        attacker = late.add_p2p_connection(FastBlocksPeer())
        for start in range(0, len(forged), MAX_HEADERS):
            attacker.send_and_ping(headers_message(forged[start:start + MAX_HEADERS]))
        newest = header_hash(forged[-1])
        assert_equal(late.getblockheader(newest)["height"], len(forged))
        assert_equal(late.getblockcount(), 0)
        # And the first block of stake with its forged signature, whole.
        first_pos = header_hash(forged[pos[0]])
        block = bytearray.fromhex(n0.getblock(first_pos, 0))
        block[112:176] = bytes(64)
        attacker.send_and_ping(MsgRawBlock(bytes(block)))

        self.log.info("Then thousands of unsigned siblings on its newest header: the block index grows by the peer's budget at most")
        tips = self.tips(late)
        slot = n0.getblock(newest)["slot"]
        # Nobody can tell yet whether they are signed: taken while the peer's budget of such headers
        # lasts (the forged ones above used some of it), then not (unchecked-budget), the peer
        # not punished.
        with late.assert_debug_log(["unchecked-budget"]):
            for k in range(SIBLINGS):
                sibling = pos_header(newest, slot + 1, merkle=k.to_bytes(32, "little"))
                attacker.send_without_ping(headers_message([sibling]))
            attacker.sync_with_ping(timeout=120)
        assert attacker.is_connected
        grown = self.tips(late) - tips
        assert_greater_than(len(grown), 0)
        assert_greater_than(UNCHECKED_PER_PEER - len(pos) + 1, len(grown))
        # A message padded with more of them, leading further than anything: none taken (what they
        # claim, nobody can check). (Slots that started; fewer than max_unanchored_slots.)
        self.wait_until(lambda: n0.getposinfo()["slot"] > slot + 60, timeout=120)
        padded = []
        prev = newest
        for k in range(50):
            padded.append(pos_header(prev, slot + 1 + k, merkle=b"\x09" * 32))
            prev = header_hash(padded[-1])
        with late.assert_debug_log(["unchecked-budget"]):
            attacker.send_and_ping(headers_message(padded))
        assert attacker.is_connected
        assert self.unknown(late, padded)

        self.log.info("An honest node connects: the node reaches the honest tip")
        for i in range(3):
            self.connect_nodes(3, i)
        # The attacker holds the first blocks, which the node asked it for. The blocks of stake after
        # them come from the honest nodes with the leaders' signatures, which the node cannot check
        # before it has connected the chain below: it takes them with its headers' signatures, and
        # does not ask for them again (it did, as fast as the peers sent them, when it turned them
        # away: blk-extension-unknown).
        time.sleep(5)
        with open(late.debug_log_path, encoding="utf-8") as log:
            assert_greater_than(100, log.read().count("state=blk-extension-unknown"))
        assert_equal(late.getblockcount(), 0)
        # The attacker leaves (the blocks it holds would otherwise wait for Bitcoin's block download
        # timeout: two minutes and more here); the node reaches the honest tip.
        attacker.peer_disconnect()
        attacker.wait_for_disconnect()
        self.wait_in_sync(self.nodes)
        assert_equal(late.getblock(first_pos, 0), n0.getblock(first_pos, 0))
        # Its stored headers carry the leaders' signatures now.
        for i in pos[:5] + pos[-5:]:
            assert_equal(late.getblockheader(header_hash(forged[i]), False), honest[i].hex())
        late.disconnect_p2ps()
        # Anchors again (blocks of stake halt after max_unanchored_slots without one).
        self.anchor()
        self.wait_in_sync(self.nodes)
        assert_peg_balance(main, self.nodes)

        self.log.info("A validator that knows the sets: unsigned headers on a deep block are a bad signature")
        anchors = [dict(a, height=n0.getblockheader(a["block"])["height"]) for a in n0.getanchorinfo(30)["recent"]]
        # The fork: the first acknowledgement (the set of its epoch is pruned); the headers acknowledge
        # every commitment after it.
        fork = min((a for a in anchors if n0.getblock(a["block"])["kind"] == "pos"), key=lambda a: a["height"])
        commitments = sorted(a["mainheight"] for a in anchors if a["mainheight"] > fork["mainheight"])
        assert_greater_than(len(commitments), 10)
        fork_slot = n0.getblock(fork["block"])["slot"]
        attack = []
        prev = fork["block"]
        for k in range(70):
            nonce = commitments[k] if k < len(commitments) else 0
            header = pos_header(prev, fork_slot + 1 + k, nonce=nonce, sections=(b"\x01" if nonce else b"\x00") + bytes(31))
            attack.append(header)
            prev = header_hash(header)
        tips = self.side_tips(n1)
        peer = n1.add_p2p_connection(FastBlocksPeer())
        with n1.assert_debug_log(["bad-pos-sig"]):
            peer.send_without_ping(headers_message(attack))
            peer.wait_for_disconnect()
        assert_equal(self.side_tips(n1), tips)
        assert self.unknown(n1, attack)

        self.log.info("Restarted, the sets forgotten and pruned: 70 unsigned acknowledging headers from deep in the chain")
        self.restart_node(2, extra_args=self.validator_args(2))
        for i in (0, 1):
            self.connect_nodes(2, i)
        self.connect_nodes(3, 2)
        self.wait_in_sync()
        tips = self.side_tips(n2)
        # (The state no longer has the set of the fork's epoch, nor does the node's memory.)
        assert not n2.getvalidators(epoch_of(fork_slot + 1))["known"]
        fork_child = n2.getblockhash(fork["height"] + 1)
        peer = n2.add_p2p_connection(FastBlocksPeer())
        with n2.assert_debug_log(["bad-pos-fork-too-deep"]):
            peer.send_and_ping(headers_message(attack))
        # Refused, not punished (its chain may be another); no new chain tip, no reorg.
        assert peer.is_connected
        assert_equal(self.side_tips(n2), tips)
        assert self.unknown(n2, attack)
        self.wait_in_sync()
        assert_equal(n2.getblockhash(fork["height"] + 1), fork_child)

        self.log.info("Thousands of unsigned siblings of a deep block: the block index does not grow")
        tips = {t["hash"] for t in n2.getchaintips()}
        for k in range(SIBLINGS):
            sibling = pos_header(fork["block"], fork_slot + 1, merkle=k.to_bytes(32, "little"))
            peer.send_without_ping(headers_message([sibling]))
        peer.sync_with_ping(timeout=120)
        assert peer.is_connected
        # No tip but the honest chain's own (it went on meanwhile).
        grown = {t["hash"] for t in n2.getchaintips()} - tips
        assert all(n2.getblockheader(h)["confirmations"] > 0 for h in grown)
        n2.disconnect_p2ps()

        self.log.info("Unsigned siblings of the tip, whose leader the node knows: a bad signature, the peer gone")
        tip = n2.getbestblockhash()
        tip_slot = n2.getblock(tip)["slot"]
        rival = pos_header(n2.getblockheader(tip)["previousblockhash"], tip_slot, merkle=b"\x07" * 32)
        peer = n2.add_p2p_connection(FastBlocksPeer())
        with n2.assert_debug_log(["bad-pos-sig"]):
            peer.send_without_ping(headers_message([rival]))
            peer.wait_for_disconnect()
        assert header_hash(rival) not in {t["hash"] for t in n2.getchaintips()}

        self.log.info("Tickets nobody committed to: 8 a peer per mainchain block")
        n0.setbmm(False)
        anchored = n1.getbestblockhash()

        def sent_tickets(tag, count):
            return [ticket_bytes(anchored, bytes([0x00, 0x14]) + bytes([tag]) * 19 + bytes([k])) for k in range(count)]

        def known(tickets):
            hashes = {t["hash"] for t in n1.getanchorinfo(100)["tickets"]}
            return sum(ticket_hash(t) in hashes for t in tickets)

        first = n1.add_p2p_connection(FastBlocksPeer())
        batch = sent_tickets(1, 12)
        for ticket in batch:
            first.send_without_ping(MsgTicket(ticket))
        first.sync_with_ping()
        assert_equal(known(batch), 8)
        # Another peer has its own 8.
        second = n1.add_p2p_connection(FastBlocksPeer())
        other = sent_tickets(2, 12)
        for ticket in other:
            second.send_without_ping(MsgTicket(ticket))
        second.sync_with_ping()
        assert_equal(known(other), 8)
        # The first peer, again in the same mainchain block: nothing more. In the next: 8 more.
        more = sent_tickets(3, 12)
        for ticket in more[:6]:
            first.send_without_ping(MsgTicket(ticket))
        first.sync_with_ping()
        assert_equal(known(more), 0)
        self.mine_main()
        for ticket in more[6:]:
            first.send_without_ping(MsgTicket(ticket))
        first.sync_with_ping()
        assert_equal(known(more), 6)
        assert first.is_connected and second.is_connected

        self.log.info("Asking for 17 tickets at once: misbehaving")
        second.send_and_ping(MsgGetTickets(ser_compact_size(16) + bytes(32 * 16)))
        assert second.is_connected
        second.send_without_ping(MsgGetTickets(ser_compact_size(17) + bytes(32 * 17)))
        second.wait_for_disconnect()
        n1.disconnect_p2ps()

        self.log.info("A proof that a leader signed twice, sent 200 times: kept once, the peer ignored")
        self.wait_in_sync()
        tip = n1.getblock(n1.getbestblockhash())
        while tip["kind"] != "pos":
            time.sleep(0.5)
            tip = n1.getblock(n1.getbestblockhash())
        header = bytes.fromhex(n1.getblockheader(tip["hash"], False))
        rival = bytearray(header)
        rival[36:68] = bytes(range(32))
        rival[112:176] = sign_schnorr(KEYS[XONLY.index(tip["proposer"])], block_sighash(rival, n1.getblockhash(0)))
        proof = MsgEvidence(header + bytes(rival))
        peer = n1.add_p2p_connection(FastBlocksPeer())
        with n1.assert_debug_log([f"validator {tip['proposer']} signed two blocks for slot {tip['slot']}"]):
            peer.send_and_ping(proof)
        with n1.assert_debug_log([], unexpected_msgs=["signed two blocks", "Misbehaving"]):
            for _ in range(199):
                peer.send_without_ping(proof)
            peer.sync_with_ping()
        assert peer.is_connected
        assert_equal(len(n1.listevidence()), 1)
        self.wait_until(lambda: all(len(n.listevidence()) == 1 for n in self.nodes), timeout=30)
        n1.disconnect_p2ps()
        self.wait_in_sync(self.nodes)
        assert_peg_balance(main, self.nodes)


if __name__ == '__main__':
    FastBlocksP2PTest(__file__).main()
