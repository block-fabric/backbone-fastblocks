#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""FastBlocks staking, as the main and test networks start: no validators, merged mined only, until
enough stake is bonded. Two nodes, each with a validator key, against a mainchain node:

  - no validators: BMM blocks carry the transactions, the producers make nothing;
  - bonds: each node's wallet bonds pegged coins to its key (the node signs the proof of
    possession); the first wallet bonds to a third key, which no node runs, with a proof made
    here (signbondpop's formula), and more stake; a BMM block carries the bonds;
  - blocks of stake from the epoch after the one after the anchor that made the set: by the
    leaders that run, the third key's slots stay empty;
  - liveness: the third key made none of the many slots it led, and is jailed: out of the sets made
    in the next epochs, its coins kept;
  - only the owner adds to a bond: a stranger's top-up of a satoshi (with the public proof of
    possession) is refused; the owner's own (which it signs) is taken;
  - unbond: the second node takes all its stake back (an unbond of all: what is bonded when it is
    mined); it leaves the stake at once, and is paid to its wallet, through the payouts of the peg
    actions, unbonding_anchors anchors later;
  - after every step, the coins of every node are the mainchain's escrow (pegbalance), bonded and
    unbonding stake included.
"""

import os
from decimal import Decimal

from feature_sidechain import ACTIVATION_PERIOD, SidechainTest
from test_framework.authproxy import JSONRPCException
from test_framework.fastblocks import KEYS, SLOT, XONLY, assert_peg_balance, bond_pop
from test_framework.messages import CTransaction, CTxOut
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error, rpc_port

UNBONDING_ANCHORS = 20
#: The tag of a bond output (OP_RETURN 0xDB'STK' | ...), in hex.
BOND_TAG = "db53544b"


class FastBlocksStakingTest(SidechainTest):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.main_rpc_port = rpc_port(self.num_nodes)
        # No -posvalidators: no validator set to start with.
        self.side_args = [
            f"-sidechainslot={SLOT}",
            f"-mainchainrpcport={self.main_rpc_port}",
            "-mainchainrpcuser=main",
            "-mainchainrpcpassword=secret",
            "-mainchainrpcwallet=miner",
        ]
        self.extra_args = [self.side_args] * self.num_nodes
        self.main_process = None

    def check_peg(self):
        return assert_peg_balance(self.main, self.nodes)

    def wait_in_sync(self):
        self.wait_until(lambda: len({n.getbestblockhash() for n in self.nodes}) == 1, timeout=60)

    def bmm_block(self):
        """A BMM block by n0, with the mempool's transactions; the mainchain commits to it."""
        self.sync_mempools()
        block = self.nodes[0].createbmmblock(self.side_address)
        self.main.createbmmrequest(SLOT, block["hash"], Decimal("0.001"))
        self.mine_main()
        self.wait_until(lambda: all(n.getbestblockhash() == block["hash"] for n in self.nodes))
        return block["hash"]

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
        n0, n1 = self.nodes
        self.main_address = main.getnewaddress()
        self.side_address = n0.getnewaddress()

        self.log.info("Activate slot 5; a deposit; the validator keys on each node: no validators, no blocks of stake")
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "FastBlocks", "Blocks of stake between merged mined blocks", "11" * 32, "22" * 20)
        self.mine_main(ACTIVATION_PERIOD + 1)
        main.createsidechaindeposit(SLOT, n0.getdepositaddress()["depositaddress"], 50)
        self.mine_main()
        self.bmm_block()
        self.payouts = [n.getnewaddress() for n in self.nodes]
        for i in range(self.num_nodes):
            path = os.path.join(self.options.tmpdir, f"validator{i}.key")
            with open(path, "w") as f:
                f.write(KEYS[i].hex() + "\n")
            self.restart_node(i, extra_args=self.side_args + [f"-validatorkeyfile={path}", f"-validatorpayout={self.payouts[i]}"])
        self.connect_nodes(0, 1)
        info = n0.getposinfo()
        assert_equal(info["active"], False)
        assert_equal(info["validators"], [])
        assert_equal(info["stalled"], True)
        # Without validators BMM blocks carry the transactions.
        txid = n0.sendtoaddress(n1.getnewaddress(), 10)
        self.bmm_block()
        assert_greater_than(n0.gettransaction(txid)["confirmations"], 0)
        assert_equal(n0.getstakinginfo()["produced"], 0)
        self.check_peg()

        self.log.info("Bonds: each wallet to its node's key; a third key, with a proof of possession made here, and more stake")
        assert_raises_rpc_error(-8, "The amount must be positive", n0.createbond, XONLY[0], 0)
        # A key the node does not hold needs its proof.
        assert_raises_rpc_error(-8, "This node does not hold the validator key", n0.createbond, XONLY[2], 1)
        bond0 = n0.createbond(XONLY[0], Decimal("0.01"))
        assert_equal(bond0["eligible"], True)
        bond1 = n1.createbond(XONLY[1], Decimal("0.01"))
        owner2 = n0.getnewaddress()
        genesis = n0.getblockhash(0)
        pop = bond_pop(KEYS[2], owner2, genesis)
        assert_equal(n1.signbondpop(n1.getnewaddress())["pubkey"], XONLY[1])
        # A proof for another owner is refused.
        assert_raises_rpc_error(-8, "The proof of possession is not the validator key's signature for this owner", n0.createbond, XONLY[2], 1, pop.hex(), n0.getnewaddress())
        bond2 = n0.createbond(XONLY[2], Decimal("0.05"), pop.hex(), owner2)
        assert_equal(bond2["owner"], owner2)
        # A BMM block carries them (still no validators): the coins leave circulation into the state.
        self.bmm_block()
        for txid in (bond0["txid"], bond2["txid"]):
            assert_greater_than(n0.gettransaction(txid)["confirmations"], 0)
        assert_greater_than(n1.gettransaction(bond1["txid"])["confirmations"], 0)
        assert_equal(sorted(b["validator"] for b in n0.listbonds()), sorted([XONLY[0], XONLY[2]]))
        assert_equal([b["validator"] for b in n1.listbonds()], [XONLY[1]])
        assert_equal(len(n1.listbonds(True)), 3)
        assert_equal(n0.getvalidator(XONLY[2])["bond"]["owner"], owner2)
        balance = self.check_peg()
        assert_equal(balance["bonded"], Decimal("0.07"))

        self.log.info("The next anchor makes a set with them; blocks of stake from the epoch after next")
        self.bmm_block()
        self.wait_until(lambda: all(n.getposinfo()["active"] for n in self.nodes), timeout=120)
        validators = {v["pubkey"]: v["stake"] for v in n0.getposinfo()["validators"]}
        assert_equal(validators, {XONLY[0]: 1000000, XONLY[1]: 1000000, XONLY[2]: 5000000})
        self.wait_until(lambda: all(n.getstakinginfo()["produced"] > 0 for n in self.nodes), timeout=120)
        self.wait_in_sync()
        # Anchors from now on: tickets the leaders acknowledge.
        n0.setbmm(True, n0.getnewaddress(), True, Decimal("0.0001"))
        self.anchor()
        self.wait_in_sync()
        self.check_peg()

        self.log.info("Liveness: the third key led most slots and made none: jailed, its coins kept")
        def jailed():
            if n0.getvalidator(XONLY[2])["bond"]["jaileduntilepoch"] > 0:
                return True
            self.anchor()
            return False
        self.wait_until(jailed, timeout=240)
        bond = n0.getvalidator(XONLY[2])["bond"]
        assert_equal(bond["bonded"], Decimal("0.05"))
        assert_equal(bond["banned"], False)
        # Out of the sets made from then on: the epochs after next have the two others.
        def without_third():
            if XONLY[2] not in [v["pubkey"] for v in n0.getposinfo()["validators"]]:
                return True
            self.anchor()
            return False
        self.wait_until(without_third, timeout=240)
        self.wait_in_sync()
        self.check_peg()

        self.log.info("Only the owner adds to a bond: a stranger's satoshi is refused, the owner's top-up taken")
        bond_tx = n1.decoderawtransaction(n1.gettransaction(bond1["txid"])["hex"])
        [bond_script] = [o["scriptPubKey"]["hex"] for o in bond_tx["vout"] if o["scriptPubKey"]["hex"].startswith("6a") and BOND_TAG in o["scriptPubKey"]["hex"][:16]]
        grief = CTransaction()
        grief.vout = [CTxOut(1, bytes.fromhex(bond_script))]
        funded = n0.fundrawtransaction(grief.serialize().hex())["hex"]
        signed = n0.signrawtransactionwithwallet(funded)["hex"]
        assert_raises_rpc_error(-26, "bad-pos-bond-owner-sig", n0.sendrawtransaction, signed, 0, Decimal("0.00000001"))
        assert_raises_rpc_error(-4, "Another wallet owns the bond of this validator", n0.createbond, XONLY[1], Decimal("0.00000001"), bond_script[2 + 4 + 8 + 64 + 40:2 + 4 + 8 + 64 + 40 + 128])
        topup = n1.createbond(XONLY[1], Decimal("0.005"))
        assert_equal(topup["bonded"], Decimal("0.015"))
        self.wait_until(lambda: n1.gettransaction(topup["txid"])["confirmations"] > 0, timeout=60)
        assert_equal(n0.getvalidator(XONLY[1])["bond"]["bonded"], Decimal("0.015"))
        self.wait_in_sync()
        self.check_peg()

        self.log.info("Unbond all: the stake leaves at once, and is paid unbonding_anchors anchors later")
        assert_raises_rpc_error(-8, "What stays bonded must be nothing or at least the minimum stake", n1.createunbond, XONLY[1], Decimal("0.01499"))
        unbond = n1.createunbond(XONLY[1], "all")
        assert_equal(unbond["amount"], Decimal("0.015"))
        assert_equal(unbond["all"], True)
        # One at a time: a second, with the same sequence, could never be mined.
        try:
            n1.createunbond(XONLY[1])
            raise AssertionError("a second unbond was made")
        except JSONRPCException as e:
            assert "waits for a block" in e.error["message"] or "nothing bonded" in e.error["message"], e.error
        owner = unbond["owner"]
        self.wait_until(lambda: n1.gettransaction(unbond["txid"])["confirmations"] > 0, timeout=60)
        listed = n1.listbonds()[0]
        assert_equal(listed["bonded"], 0)
        assert_equal(listed["unbonding"], Decimal("0.015"))
        release = listed["unbonds"][0]["releaseanchor"]
        assert_equal(release - n1.getposinfo()["anchors"] <= UNBONDING_ANCHORS, True)
        received = n1.getreceivedbyaddress(owner, 0)
        self.check_peg()
        while n1.getposinfo()["anchors"] < release:
            assert_equal(n1.getreceivedbyaddress(owner, 0), received)
            self.anchor()
        self.wait_until(lambda: n1.getreceivedbyaddress(owner, 0) - received == Decimal("0.015"), timeout=60)
        assert_equal(n1.listbonds()[0]["unbonding"], 0)
        self.wait_in_sync()
        balance = self.check_peg()
        assert_equal(balance["bonded"], Decimal("0.06"))
        assert_equal(balance["unbonding"], 0)
        # Blocks of stake go on with the first key.
        height = n0.getblockcount()
        self.wait_until(lambda: n0.getblockcount() > height + 2, timeout=60)
        self.wait_in_sync()
        self.check_peg()


if __name__ == '__main__':
    FastBlocksStakingTest(__file__).main()
