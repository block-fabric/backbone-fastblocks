#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""FastBlocks soak test (extended; --duration=<minutes>, 30 by default): the real networks' 30-second
slots (-posslotseconds=30), so that what happens at the boundary of a slot happens as it does there,
not as mock time or one-second slots make it. Three validators and an observer that merged mines; in
rounds of a slot or two, one of: a node restarted (or killed), a validator cut off from the others for a
while, a mainchain reorg, a bond, an unbond, a payment, a withdrawal. After each round: the nodes agree
on one tip, every node's coins are the escrow (pegbalance), no node died, and the slots the validators
failed stay few.
"""

import random
import time
from decimal import Decimal

from feature_sidechain import SidechainTest
from test_framework.authproxy import JSONRPCException
from test_framework.fastblocks import (
    SLOT,
    XONLY,
    activate_and_fund,
    assert_peg_balance,
    write_key_file,
)
from test_framework.util import assert_equal, rpc_port

SLOT_SECONDS = 30


class FastBlocksSoakTest(SidechainTest):
    def add_options(self, parser):
        parser.add_argument("--duration", dest="duration", type=float, default=30, help="How long to run, in minutes (default: 30)")

    def set_test_params(self):
        self.num_nodes = 4
        self.setup_clean_chain = True
        self.rpc_timeout = 240
        self.main_rpc_port = rpc_port(self.num_nodes)
        self.side_args = [
            f"-sidechainslot={SLOT}",
            f"-mainchainrpcport={self.main_rpc_port}",
            "-mainchainrpcuser=main",
            "-mainchainrpcpassword=secret",
            "-mainchainrpcwallet=miner",
            f"-posvalidators={','.join(XONLY)}",
            f"-posslotseconds={SLOT_SECONDS}",
        ]
        self.extra_args = [self.side_args] * self.num_nodes
        self.main_process = None

    def setup_network(self):
        self.start_mainchain()
        self.setup_nodes()
        self.connect_all()

    def connect_all(self):
        for a in range(self.num_nodes):
            for b in range(a + 1, self.num_nodes):
                self.connect_nodes(a, b)

    def connect_to_all(self, i):
        for other in range(self.num_nodes):
            if other != i:
                self.connect_nodes(i, other)

    def mine_main(self, count=1):
        hashes = self.main.generatetoaddress(count, self.main_address)
        for node in self.nodes:
            if node.running:
                node.syncmainchain()
        return hashes

    def node_args(self, i):
        if i < 3:
            return self.side_args + [f"-validatorkeyfile={self.key_files[i]}", f"-validatorpayout={self.payouts[i]}"]
        return self.side_args

    def wait_one_tip(self, timeout=6 * SLOT_SECONDS):
        self.wait_until(lambda: len({n.getbestblockhash() for n in self.nodes}) == 1, timeout=timeout)

    def run_test(self):
        try:
            self.run_sidechain_test()
        finally:
            self.stop_mainchain()

    def start_observer_bmm(self):
        self.nodes[3].setbmm(True, self.observer_address, True, Decimal("0.0001"))

    #
    # Events
    #

    def restart(self):
        i = random.randrange(self.num_nodes)
        node = self.nodes[i]
        if random.random() < 0.5:
            self.log.info(f"  node {i} killed and started again")
            node.process.kill()
            node.wait_until_stopped(expected_ret_code=-9)
            self.start_node(i, extra_args=self.node_args(i))
        else:
            self.log.info(f"  node {i} restarted")
            self.restart_node(i, extra_args=self.node_args(i))
        self.connect_to_all(i)
        if i == 3:
            self.start_observer_bmm()

    def partition(self):
        i = random.randrange(3)
        slots = random.randint(1, 3)
        self.log.info(f"  validator {i} cut off for {slots} slot(s)")
        for other in range(self.num_nodes):
            if other != i:
                self.disconnect_nodes(i, other)
        time.sleep(slots * SLOT_SECONDS)
        self.connect_to_all(i)

    def main_reorg(self):
        depth = random.randint(1, 2)
        self.log.info(f"  a mainchain reorg of {depth} block(s)")
        main = self.main
        main.invalidateblock(main.getblockhash(main.getblockcount() - depth + 1))
        self.mine_main(depth + 1)

    def bond(self):
        i = random.randrange(3)
        amount = Decimal(random.randint(1, 50)) / 1000
        self.log.info(f"  validator {i} bonds {amount}")
        try:
            self.nodes[i].createbond(XONLY[i], amount)
        except JSONRPCException as e:
            self.log.info(f"    refused: {e.error['message']}")

    def unbond(self):
        i = random.randrange(3)
        self.log.info(f"  validator {i} unbonds")
        try:
            if random.random() < 0.5:
                self.nodes[i].createunbond(XONLY[i])
            else:
                self.nodes[i].createunbond(XONLY[i], Decimal("0.001"))
        except JSONRPCException as e:
            self.log.info(f"    refused: {e.error['message']}")

    def payment(self):
        a, b = random.sample(range(self.num_nodes), 2)
        amount = Decimal(random.randint(1, 100)) / 100
        self.log.info(f"  node {a} pays node {b} {amount}")
        try:
            self.nodes[a].sendtoaddress(self.nodes[b].getnewaddress(), amount)
        except JSONRPCException as e:
            self.log.info(f"    refused: {e.error['message']}")

    def withdrawal(self):
        i = random.randrange(self.num_nodes)
        amount = Decimal(random.randint(1, 50)) / 100
        self.log.info(f"  node {i} withdraws {amount}")
        try:
            self.nodes[i].createwithdrawal(self.main.getnewaddress(), amount, Decimal("0.01"))
        except JSONRPCException as e:
            self.log.info(f"    refused: {e.error['message']}")

    #
    # After each round
    #

    def check(self, round_):
        for i, node in enumerate(self.nodes):
            assert node.process.poll() is None, f"node {i} died"
        self.wait_one_tip()
        assert_peg_balance(self.main, self.nodes)
        for node in self.nodes:
            assert_equal(node.listevidence(), [])
        info = [n.getstakinginfo() for n in self.nodes[:3]]
        self.log.info(f"round {round_}: height {self.nodes[0].getblockcount()}, anchors {self.nodes[0].getposinfo()['anchors']}, "
                      f"produced {[s['produced'] for s in info]}, failed {[s['failed'] for s in info]}")
        # The slots a validator led and made no block for, since it started: few (a restart, a
        # partition, a reorg costs one or two).
        for s in info:
            assert s["failed"] <= 3 + s["produced"] // 4, s

    def run_sidechain_test(self):
        main = self.main
        n0, n1, n2, observer = self.nodes
        self.main_address = main.getnewaddress()
        self.observer_address = observer.getnewaddress()

        self.log.info("Activate slot 5, a deposit; the validators start, the observer merged mines")
        activate_and_fund(self, funded=0)
        self.key_files = [write_key_file(self, i) for i in range(3)]
        self.payouts = [n.getnewaddress() for n in self.nodes[:3]]
        for i in range(3):
            self.restart_node(i, extra_args=self.node_args(i))
        self.connect_all()
        for n in (n1, n2, observer):
            n0.sendtoaddress(n.getnewaddress(), 8)
        self.start_observer_bmm()
        self.wait_until(lambda: all(n.getstakinginfo()["produced"] > 0 for n in self.nodes[:3]), timeout=10 * SLOT_SECONDS)
        self.mine_main()
        self.wait_one_tip()

        events = [self.restart, self.partition, self.main_reorg, self.bond, self.unbond, self.payment, self.withdrawal]
        end = time.time() + self.options.duration * 60
        round_ = 0
        while time.time() < end:
            round_ += 1
            self.log.info(f"Round {round_}")
            random.choice(events)()
            # A mainchain block a slot or so: anchors, deposits, bundles.
            time.sleep(random.uniform(0.5, 1.5) * SLOT_SECONDS)
            self.mine_main()
            self.check(round_)
        self.log.info(f"{round_} rounds in {self.options.duration} minutes")


if __name__ == '__main__':
    FastBlocksSoakTest(__file__).main()
