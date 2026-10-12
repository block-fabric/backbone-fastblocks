#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""FastBlocks helpers for functional tests: the validator keys of the tests, the peg balance
against the mainchain's escrow, and blocks of stake as bytes (to sign a second one for a slot)."""

import time
from decimal import Decimal
from io import BytesIO

from .key import TaggedHash, compute_xonly_pubkey, sign_schnorr
from .segwit_addr import decode_segwit_address
from .messages import CBlock, CTransaction, deser_compact_size, deser_vector, hash256, ser_compact_size, ser_uint256, ser_vector
from .util import assert_equal

SLOT = 5
POS_BLOCK_BIT = 0x10000
KEYS = [bytes([0x11] + [0] * 30 + [i]) for i in (1, 2, 3)]
XONLY = [compute_xonly_pubkey(key)[0].hex() for key in KEYS]
#: The size of the header of a block of stake: Bitcoin's 80 bytes, hashSections and the signature.
POS_HEADER_SIZE = 176


def escrow_at(main, height):
    """What the mainchain held in escrow for the slot after its block at `height` (0 before any deposit)."""
    total = Decimal(0)
    tip = main.getblockcount()
    for change in main.listsidechaindeposits(SLOT):
        if tip - change["confirmations"] + 1 <= height:
            total = change["total"]
    return total


def assert_peg_balance(main, nodes):
    """Design 6.3: every node's coins (spendable, withdrawals, the pot, bonded and unbonding stake, the
    pot's reserve of penalties, rewards and payouts owed) are the escrow on the mainchain after the
    mainchain block it acted on, up to. (The tests leave no fee unclaimed and burn no coins.)"""
    for node in nodes:
        node.syncmainchain()
        balance = node.getsidechainstate()["pegbalance"]
        parts = ("utxos", "withdrawals", "pot", "bonded", "unbonding", "reserve", "rewards", "payouts")
        assert_equal(balance["total"], sum(balance[part] for part in parts))
        assert_equal(balance["total"], escrow_at(main, balance["mainheight"]))
    return balance


def stop_bmm(main, main_address, node, nodes):
    """The merged miner of `node` off, and the ticket requests it made meanwhile dropped: a mainchain
    block without them makes them stale. (Its follower may be making one as it is switched off: the
    mempool has to stay empty for a while.)"""
    node.setbmm(False)
    quiet = 0
    while quiet < 3:
        time.sleep(0.5)
        if main.getrawmempool():
            main.generateblock(main_address, [])
            for n in nodes:
                n.syncmainchain()
            quiet = 0
        else:
            quiet += 1
    assert_equal(main.getrawmempool(), [])


class PosBlock:
    """A block of stake as bytes: its extended header, its transactions, its sections."""

    def __init__(self, raw_hex):
        f = BytesIO(bytes.fromhex(raw_hex))
        self.header = bytearray(f.read(POS_HEADER_SIZE))
        assert int.from_bytes(self.header[0:4], "little") & POS_BLOCK_BIT
        self.vtx = deser_vector(f, CTransaction)
        self.sections = f.read(deser_compact_size(f))
        assert_equal(f.read(), b"")

    @property
    def hash(self):
        return hash256(bytes(self.header[:112]))[::-1].hex()

    def resign(self, key):
        """The merkle root of the transactions, and the signature of `key` over the block hash."""
        block = CBlock()
        block.vtx = self.vtx
        self.header[36:68] = ser_uint256(block.calc_merkle_root())
        self.header[112:176] = sign_schnorr(key, TaggedHash("hpos/block", hash256(bytes(self.header[:112]))))

    def serialize(self):
        return bytes(self.header) + ser_vector(self.vtx, "serialize_with_witness") + ser_compact_size(len(self.sections)) + self.sections


def bond_pop(key, owner_address, genesis_hash, hrp="rsc"):
    """The proof of possession a bond to `key` owned by `owner_address` (P2WPKH) needs: the key's
    BIP340 signature of TaggedHash("hpos/bond", owner key hash || genesis hash)."""
    version, program = decode_segwit_address(hrp, owner_address)
    assert version == 0 and len(program) == 20
    return sign_schnorr(key, TaggedHash("hpos/bond", bytes(program) + bytes.fromhex(genesis_hash)[::-1]))
