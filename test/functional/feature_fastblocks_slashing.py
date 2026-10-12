#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""FastBlocks slashing: a leader that signs two blocks for a slot loses 10 % of its stake and is banned
for good; the block that carries the proof is paid 1/8 of the penalty. Three validators (the genesis
set, a bootstrap of regtest), one key per node, each with a bond:

  - equivocation: a leader signs a second block for a slot it made a block of; the nodes find the
    proof in the headers, and a leader puts it in a block of stake;
  - the penalty: 10 % of the offender's stake goes, the rest is unbonded; it is banned (no set, no
    bond); the reward is owed to the script of the first output of the block that carried the proof,
    and paid at the next peg step, with a share of the penalty's rest (the reserve) to the anchor's payee;
  - invalid evidence stays refused, and a proof is punished once;
  - after every step, the coins of every node are the mainchain's escrow (pegbalance), the reserve
    and the rewards owed included.
"""

import os
from decimal import Decimal

from feature_sidechain import ACTIVATION_PERIOD, SidechainTest
from test_framework.fastblocks import KEYS, SLOT, XONLY, PosBlock, assert_peg_balance
from test_framework.messages import CTxIn
from test_framework.script import CScript
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error, rpc_port

BOND = Decimal("1")
PENALTY = BOND / 10
REWARD = PENALTY / 8


class FastBlocksSlashingTest(SidechainTest):
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
            f"-posvalidators={','.join(XONLY)}",
        ]
        self.extra_args = [self.side_args] * self.num_nodes
        self.main_process = None

    def connect_all(self):
        for a, b in ((0, 1), (1, 2), (0, 2)):
            self.connect_nodes(a, b)

    def wait_in_sync(self):
        self.wait_until(lambda: len({n.getbestblockhash() for n in self.nodes}) == 1, timeout=60)

    def check_peg(self):
        return assert_peg_balance(self.main, self.nodes)

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
        n0, n1, n2 = self.nodes
        self.main_address = main.getnewaddress()

        self.log.info("Activate slot 5; a deposit; the validators start; each bonds to its key")
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "FastBlocks", "Blocks of stake between merged mined blocks", "11" * 32, "22" * 20)
        self.mine_main(ACTIVATION_PERIOD + 1)
        main.createsidechaindeposit(SLOT, n0.getdepositaddress()["depositaddress"], 50)
        self.mine_main()
        block = n0.createbmmblock(n0.getnewaddress())
        main.createbmmrequest(SLOT, block["hash"], Decimal("0.001"))
        self.mine_main()
        self.wait_until(lambda: all(n.getbestblockhash() == block["hash"] for n in self.nodes))
        self.payouts = [n.getnewaddress() for n in self.nodes]
        for i, key in enumerate(KEYS):
            path = os.path.join(self.options.tmpdir, f"validator{i}.key")
            with open(path, "w") as f:
                f.write(key.hex() + "\n")
            self.restart_node(i, extra_args=self.side_args + [f"-validatorkeyfile={path}", f"-validatorpayout={self.payouts[i]}"])
        self.connect_all()
        self.wait_until(lambda: all(n.getstakinginfo()["produced"] > 0 for n in self.nodes), timeout=120)
        for n in (n1, n2):
            txid = n0.sendtoaddress(n.getnewaddress(), 10)
            self.wait_until(lambda: n0.gettransaction(txid)["confirmations"] > 0, timeout=60)
        self.wait_in_sync()
        for i, n in enumerate(self.nodes):
            n.createbond(XONLY[i], BOND)
        self.wait_until(lambda: all(n.getposinfo()["staking"]["bonded"] == 3 * BOND for n in self.nodes), timeout=60)
        n0.setbmm(True, n0.getnewaddress(), True, Decimal("0.0001"))
        self.anchor()
        self.wait_in_sync()
        self.check_peg()

        self.log.info("Equivocation: a leader signs a second block for a slot it made a block of")
        tip = n0.getblock(n0.getbestblockhash())
        while tip["kind"] != "pos":
            self.wait_until(lambda: n0.getbestblockhash() != tip["hash"], timeout=30)
            tip = n0.getblock(n0.getbestblockhash())
        offender = tip["proposer"]
        original = PosBlock(n0.getblock(tip["hash"], 0))
        rival = PosBlock(n0.getblock(tip["hash"], 0))
        coinbase = rival.vtx[0]
        coinbase.vin[0] = CTxIn(coinbase.vin[0].prevout, CScript(bytes(coinbase.vin[0].scriptSig) + bytes([1, 0x42])), coinbase.vin[0].nSequence)
        rival.resign(KEYS[XONLY.index(offender)])
        assert rival.hash != original.hash
        # Invalid evidence stays refused: the second header signed by another key is no proof.
        forged = PosBlock(n0.getblock(tip["hash"], 0))
        forged.vtx = rival.vtx
        forged.resign(KEYS[(XONLY.index(offender) + 1) % 3])
        assert_raises_rpc_error(-26, "bad-pos-evidence-sig", n0.submitevidence, bytes(original.header).hex() + bytes(forged.header).hex())
        # The rival block, on the same parent: a branch; the node sees two headers its leader signed.
        assert_equal(n1.submitblock(rival.serialize().hex()), "inconclusive")
        self.wait_until(lambda: all(len(n.listevidence()) == 1 for n in self.nodes), timeout=60)

        self.log.info("A leader puts the proof in a block: 10 % of the offender's stake goes, the rest is unbonded, it is banned")
        self.wait_until(lambda: all(n.getvalidator(offender)["bond"]["banned"] for n in self.nodes), timeout=60)
        self.wait_in_sync()
        bond = n0.getvalidator(offender)["bond"]
        assert_equal(bond["bonded"], 0)
        assert_equal(bond["unbonding"], BOND - PENALTY)
        assert_equal(bond["eligible"], False)
        staking = n0.getposinfo()["staking"]
        assert_equal(staking["slashed"], PENALTY)
        assert_equal(staking["bonded"], 2 * BOND)
        assert_equal(staking["unbonding"], BOND - PENALTY)
        # The block that carried it, and who it pays: its first output.
        carrying = n0.getblock(n0.getbestblockhash(), 2)
        while "evidence" not in carrying:
            carrying = n0.getblock(carrying["previousblockhash"], 2)
        assert_equal(carrying["evidence"][0]["slot"], tip["slot"])
        assert_equal(sorted(carrying["evidence"][0]["hashes"]), sorted([original.hash, rival.hash]))
        reporter = carrying["tx"][0]["vout"][0]["scriptPubKey"]["hex"]
        self.check_peg()
        # Not twice: the same proof is no longer one this chain takes, and the offender's key no longer bonds.
        assert_raises_rpc_error(-8, "This validator is banned", n0.createbond, offender, BOND)

        self.log.info("The next peg step pays the reward to the block that carried the proof")
        self.anchor()
        self.anchor()
        self.wait_in_sync()

        def pays_reward(blockhash):
            block = n0.getblock(blockhash, 2)
            return any(out["value"] == REWARD and out["scriptPubKey"]["hex"] == reporter for out in block["tx"][0]["vout"])
        hashes = []
        blockhash = n0.getbestblockhash()
        while blockhash != carrying["hash"]:
            hashes.append(blockhash)
            blockhash = n0.getblock(blockhash)["previousblockhash"]
        assert_equal(sum(pays_reward(h) for h in hashes), 1)
        staking = n0.getposinfo()["staking"]
        assert_equal(staking["rewardsowed"], 0)
        # The rest of the penalty is in the reserve, paid out a share per anchor with the pot.
        reserve = n0.getposinfo()["pot"]["reserve"]
        assert_greater_than(PENALTY - REWARD, reserve)
        self.check_peg()

        self.log.info("The offender leaves the validator sets made from then on")
        def offender_gone():
            if offender not in [v["pubkey"] for v in n0.getposinfo()["validators"]]:
                return True
            self.anchor()
            return False
        self.wait_until(offender_gone, timeout=240)
        self.wait_in_sync()
        self.check_peg()


if __name__ == '__main__':
    FastBlocksSlashingTest(__file__).main()
