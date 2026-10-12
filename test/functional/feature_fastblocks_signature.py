#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""FastBlocks: the leader's signature, which the block hash does not cover, cannot be swapped.

A node that has the header of a block of stake turns away a block with that hash and another
signature (another key's, or none) as a mutated block, whether it comes whole or as a compact
block, without taking the header for invalid; the block its leader signed is taken after, and
stays so through a -reindex.

The producer signs deterministically (BIP340 with all-zero auxiliary randomness). Another valid
signature of the leader over a block (made with other randomness) is the same block: a node that has
the header with one takes the block with the other, keeps its own, and disconnects nobody; two nodes
that hold the two stay together.
"""

import os

from decimal import Decimal

from feature_sidechain import ACTIVATION_PERIOD, SidechainTest
from test_framework.fastblocks import KEYS, SLOT, XONLY, PosBlock
from test_framework.key import TaggedHash, sign_schnorr
from test_framework.messages import hash256, msg_sendcmpct, ser_compact_size
from test_framework.p2p import P2PInterface
from test_framework.util import assert_equal, assert_raises_rpc_error, rpc_port


class RawBlock:
    """A block message, as bytes (the framework's block has Bitcoin's 80-byte header)."""
    msgtype = b"block"

    def __init__(self, data):
        self.data = data

    def serialize(self):
        return self.data


class RawCompactBlock(RawBlock):
    msgtype = b"cmpctblock"


def compact_block(block):
    """A compact block with every transaction prefilled (BIP 152), and the sections of an extended block."""
    data = bytes(block.header) + (42).to_bytes(8, "little") + ser_compact_size(0) + ser_compact_size(len(block.vtx))
    for tx in block.vtx:
        # Each index is the difference from the last one, plus one: 0 when all are prefilled.
        data += ser_compact_size(0) + tx.serialize_with_witness()
    return data + ser_compact_size(len(block.sections)) + block.sections


class FastBlocksSignatureTest(SidechainTest):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.main_rpc_port = rpc_port(self.num_nodes)
        self.side_args = [
            f"-sidechainslot={SLOT}",
            f"-mainchainrpcport={self.main_rpc_port}",
            "-mainchainrpcuser=main",
            "-mainchainrpcpassword=secret",
            "-mainchainrpcwallet=miner",
            f"-posvalidators={XONLY[0]}",
        ]
        self.extra_args = [self.side_args] * self.num_nodes
        self.main_process = None

    def run_test(self):
        try:
            self.run_sidechain_test()
        finally:
            self.stop_mainchain()

    def with_signature(self, honest, sig):
        block = PosBlock(honest.serialize().hex())
        block.header[112:176] = sig
        assert_equal(block.hash, honest.hash)
        return block

    def run_sidechain_test(self):
        main = self.main
        n0, n1 = self.nodes
        self.main_address = main.getnewaddress()

        self.log.info("Activate slot 5; a BMM block; node 0 the only validator")
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "FastBlocks", "Blocks of stake between merged mined blocks", "11" * 32, "22" * 20)
        self.mine_main(ACTIVATION_PERIOD + 1)
        block = n0.createbmmblock(n0.getnewaddress())
        main.createbmmrequest(SLOT, block["hash"], Decimal("0.001"))
        self.mine_main()
        self.wait_until(lambda: all(n.getbestblockhash() == block["hash"] for n in self.nodes))
        key_file = self.options.tmpdir + "/validator0.key"
        with open(key_file, "w") as f:
            f.write(KEYS[0].hex() + "\n")
        self.restart_node(0, extra_args=self.side_args + [f"-validatorkeyfile={key_file}"])
        self.connect_nodes(0, 1)
        height = n0.getblockcount()
        self.wait_until(lambda: n1.getblockcount() >= height + 3, timeout=60)

        self.log.info("Node 1 has the header of node 0's newest block of stake, not the block")
        self.disconnect_nodes(0, 1)
        # Node 0 is the only validator: node 1's tip is on its chain, and node 0 goes on.
        base = n1.getblockcount()
        self.wait_until(lambda: n0.getblockcount() > base, timeout=30)
        target = n0.getblockhash(base + 1)
        assert_equal(n0.getblockheader(target)["previousblockhash"], n1.getbestblockhash())
        honest = PosBlock(n0.getblock(target, 0))
        assert_equal(honest.hash, target)
        n1.submitheader(bytes(honest.header).hex())
        assert_equal(n1.getblockheader(target)["hash"], target)

        other = self.with_signature(honest, sign_schnorr(KEYS[1], TaggedHash("hpos/block", hash256(bytes(honest.header[:112])))))
        zeroed = self.with_signature(honest, bytes(64))

        self.log.info("A block with the hash and another key's signature, relayed: a mutated block")
        peer = n1.add_p2p_connection(P2PInterface())
        with n1.assert_debug_log(["bad-blk-extension"]):
            peer.send_without_ping(RawBlock(other.serialize()))
            peer.wait_for_disconnect()
        assert_raises_rpc_error(-1, "Block not available", n1.getblock, target)
        tips = {t["hash"]: t["status"] for t in n1.getchaintips()}
        assert_equal(tips[target], "headers-only")

        self.log.info("As a compact block, with no signature: not reconstructed")
        peer = n1.add_p2p_connection(P2PInterface())
        peer.send_and_ping(msg_sendcmpct(announce=False, version=2))
        with n1.assert_debug_log(["without the extension of its header"]):
            peer.send_and_ping(RawCompactBlock(compact_block(zeroed)))
        assert_raises_rpc_error(-1, "Block not available", n1.getblock, target)
        n1.disconnect_p2ps()

        self.log.info("With no signature, submitted: the same")
        assert_equal(n1.submitblock(zeroed.serialize().hex()), "bad-blk-extension")
        assert_equal({t["hash"]: t["status"] for t in n1.getchaintips()}[target], "headers-only")

        self.log.info("The block its leader signed, relayed after: taken")
        peer = n1.add_p2p_connection(P2PInterface())
        peer.send_and_ping(RawBlock(honest.serialize()))
        self.wait_until(lambda: n1.getbestblockhash() == target, timeout=30)
        assert_equal(n1.getblock(target, 0), honest.serialize().hex())
        n1.disconnect_p2ps()

        self.log.info("A block with another signature, once the block is stored: still a mutated one")
        assert_equal(n1.submitblock(other.serialize().hex()), "bad-blk-extension")
        assert_equal(n1.getblock(target, 0), honest.serialize().hex())

        self.log.info("-reindex: the block, with its leader's signature")
        self.restart_node(1, extra_args=self.side_args + ["-reindex"])
        self.wait_until(lambda: n1.getblockcount() >= n1.getblockheader(target)["height"], timeout=60)
        assert_equal(n1.getblock(target, 0), honest.serialize().hex())
        assert_equal(n1.getblockheader(target)["confirmations"] > 0, True)

        self.log.info("The producer signs deterministically: BIP340 with all-zero auxiliary randomness")
        height = n1.getblockheader(target)["height"]
        self.wait_until(lambda: n0.getblockcount() >= height + 2, timeout=60)
        honest2 = PosBlock(n0.getblock(n0.getblockhash(height + 1), 0))
        honest3 = PosBlock(n0.getblock(n0.getblockhash(height + 2), 0))
        for block in (honest, honest2, honest3):
            assert_equal(bytes(block.header[112:176]), sign_schnorr(KEYS[0], TaggedHash("hpos/block", hash256(bytes(block.header[:112])))))

        def resigned(block):
            """The block with another valid signature of its leader: other auxiliary randomness."""
            again = self.with_signature(block, sign_schnorr(KEYS[0], TaggedHash("hpos/block", hash256(bytes(block.header[:112]))), aux=os.urandom(32)))
            assert again.header[112:176] != block.header[112:176]
            return again
        other2, other3 = resigned(honest2), resigned(honest3)

        self.log.info("Another valid signature of the leader: the same block, taken, the node's own kept, no peer punished")
        n1.submitheader(bytes(other2.header).hex())
        peer = n1.add_p2p_connection(P2PInterface())
        peer.send_and_ping(RawBlock(honest2.serialize()))
        self.wait_until(lambda: n1.getbestblockhash() == honest2.hash, timeout=30)
        assert_equal(n1.getblock(honest2.hash, 0), other2.serialize().hex())
        # Again, as a compact block, and submitted: a duplicate, nothing changes, the peer stays.
        peer.send_and_ping(msg_sendcmpct(announce=False, version=2))
        peer.send_and_ping(RawCompactBlock(compact_block(honest2)))
        peer.send_and_ping(RawBlock(honest2.serialize()))
        assert peer.is_connected
        assert_equal(n1.submitblock(honest2.serialize().hex()), "duplicate")
        assert_equal(n1.getblock(honest2.hash, 0), other2.serialize().hex())
        n1.disconnect_p2ps()

        self.log.info("Two nodes holding two valid signatures of one block: no disconnect, no split")
        n1.submitheader(bytes(other3.header).hex())
        self.connect_nodes(0, 1)
        self.wait_until(lambda: n0.getbestblockhash() == n1.getbestblockhash(), timeout=60)
        assert_equal(n1.getblock(honest3.hash, 0), other3.serialize().hex())
        assert_equal(n0.getblock(honest3.hash, 0), honest3.serialize().hex())
        tip = n0.getblockcount()
        self.wait_until(lambda: n0.getblockcount() >= tip + 3 and n0.getbestblockhash() == n1.getbestblockhash(), timeout=60)
        assert_equal(len(n0.getpeerinfo()), 1)
        assert_equal(len(n1.getpeerinfo()), 1)


if __name__ == "__main__":
    FastBlocksSignatureTest(__file__).main()
