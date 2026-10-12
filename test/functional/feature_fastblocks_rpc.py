#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""FastBlocks' RPCs, P2P messages and catching up, with three validators and an observer.

  - the validator set, the leader schedule, a validator, staking on and off;
  - anchors and finality: getanchorinfo, getblock's kind, slot, proposer, acknowledgement
    and finality, getblockchaininfo's anchored and final tips;
  - the wallet: gettransaction, listtransactions and getbalances say how final coins are;
  - evidence: a proof that a leader signed two blocks for one slot, submitted to one node,
    reaches the others; a malformed or false one is refused;
  - tickets: a peer asks for the ticket of a commitment (fbgettickets) and gets it;
  - an observer that joins late takes in the whole chain, blocks of stake, acknowledgements
    and BMM blocks, and agrees with the validators.
"""

import os
from decimal import Decimal

from feature_sidechain import ACTIVATION_PERIOD, SidechainTest
from test_framework.fastblocks import assert_peg_balance
from test_framework.key import TaggedHash, compute_xonly_pubkey, sign_schnorr
from test_framework.messages import hash256, ser_compact_size
from test_framework.p2p import MESSAGEMAP, P2PInterface
from test_framework.util import assert_equal, assert_greater_than, assert_greater_than_or_equal, assert_raises_rpc_error, rpc_port

SLOT = 5
KEYS = [bytes([0x11] + [0] * 30 + [i]) for i in (1, 2, 3)]
XONLY = [compute_xonly_pubkey(key)[0].hex() for key in KEYS]
RPC_VERIFY_REJECTED = -26
RPC_DESERIALIZATION_ERROR = -22
RPC_MISC_ERROR = -1


class RawMessage:
    """A FastBlocks message, kept as its bytes."""
    msgtype = b""

    def __init__(self, data=b""):
        self.data = data

    def deserialize(self, f):
        self.data = f.read()

    def serialize(self):
        return self.data

    def __repr__(self):
        return f"{self.msgtype.decode()}({self.data.hex()})"


class MsgTicket(RawMessage):
    msgtype = b"fbticket"


class MsgGetTickets(RawMessage):
    msgtype = b"fbgettickets"


class MsgEvidence(RawMessage):
    msgtype = b"fbevidence"


for message in (MsgTicket, MsgGetTickets, MsgEvidence):
    MESSAGEMAP[message.msgtype] = message


class FastBlocksPeer(P2PInterface):
    def __init__(self):
        super().__init__()
        self.tickets = []

    def on_fbticket(self, message):
        self.tickets.append(message.data)

    def on_fbgettickets(self, message):
        pass

    def on_fbevidence(self, message):
        pass


def ticket_hash(ticket):
    """h* of a serialized ticket."""
    return TaggedHash("hpos/ticket", ticket)[::-1].hex()


class FastBlocksRpcTest(SidechainTest):
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
        # Node 3, the observer, joins later.
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
        self.wait_until(lambda: len({n.getbestblockhash() for n in nodes}) == 1, timeout=60)

    def anchor(self):
        """One more anchor: wait for the merged miner's ticket request, mine it, wait for its acknowledgement."""
        before = [n.getposinfo()["anchors"] for n in self.validators()]
        self.wait_until(lambda: len(self.main.getrawmempool()) > 0, timeout=60)
        self.mine_main()
        self.wait_until(lambda: all(n.getposinfo()["anchors"] > b for n, b in zip(self.validators(), before)), timeout=60)

    def run_test(self):
        try:
            self.run_sidechain_test()
        finally:
            self.stop_mainchain()

    def run_sidechain_test(self):
        main = self.main
        n0, n1, n2, observer = self.nodes
        self.main_address = main.getnewaddress()

        self.log.info("Activate slot 5 on the mainchain; a deposit in a BMM block")
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "FastBlocks", "Blocks of stake between merged mined blocks", "11" * 32, "22" * 20)
        self.mine_main(ACTIVATION_PERIOD + 1)
        main.createsidechaindeposit(SLOT, n0.getdepositaddress()["depositaddress"], 50)
        self.mine_main()
        block = n0.createbmmblock(n0.getnewaddress())
        main.createbmmrequest(SLOT, block["hash"], Decimal("0.001"))
        self.mine_main()
        self.wait_until(lambda: all(n.getbestblockhash() == block["hash"] for n in self.validators()))
        assert_equal(n0.getblock(block["hash"])["kind"], "bmm")
        assert_peg_balance(main, self.validators())

        self.log.info("Before any validator runs: the set and the schedule, no staking")
        info = n0.getvalidators()
        assert_equal(info["active"], True)
        assert_equal(info["totalstake"], 3)
        assert_equal(sorted(v["pubkey"] for v in info["validators"]), sorted(XONLY))
        assert_equal(sum(v["slots"] for v in info["validators"]), 20)
        assert all(not v["local"] for v in info["validators"])
        assert_equal(n0.getvalidators(info["epoch"] + 1)["epoch"], info["epoch"] + 1)
        schedule = n0.getleaderschedule(info["epoch"])
        assert_equal(len(schedule["slots"]), 20)
        assert_equal(schedule["slots"][0]["slot"], info["firstslot"])
        assert all(s["leader"] in XONLY for s in schedule["slots"])
        assert_equal(schedule["slots"][1]["time"] - schedule["slots"][0]["time"], 1)
        assert_raises_rpc_error(-8, "epoch out of range", n0.getleaderschedule, -1)
        staking = n0.getstakinginfo()
        assert_equal(staking["validator"], False)
        assert_equal(staking["enabled"], False)
        assert_raises_rpc_error(RPC_MISC_ERROR, "no validator keys", n0.setstaking, True)
        validator = n0.getvalidator(XONLY[1])
        assert_equal(validator["inset"], True)
        assert_equal(validator["stake"], 1)
        assert_equal(validator["local"], False)
        assert_equal(n0.getvalidator("33" * 32)["inset"], False)
        assert_raises_rpc_error(-8, "x-only key", n0.getvalidator, "33")

        self.log.info("Start the validators")
        self.payouts = [n.getnewaddress() for n in self.validators()]
        for i, key in enumerate(KEYS):
            path = os.path.join(self.options.tmpdir, f"validator{i}.key")
            with open(path, "w") as f:
                f.write(key.hex() + "\n")
            self.restart_node(i, extra_args=self.side_args + [f"-validatorkeyfile={path}", f"-validatorpayout={self.payouts[i]}"])
        self.connect_validators()
        self.wait_until(lambda: all(n.getstakinginfo()["produced"] > 0 for n in self.validators()), timeout=120)
        self.wait_in_sync()
        staking = n0.getstakinginfo()
        assert_equal(staking["validator"], True)
        assert_equal(staking["enabled"], True)
        assert_equal(staking["keys"], [{"pubkey": XONLY[0], "inset": True, "stake": 1}])
        assert_equal((staking["stake"], staking["totalstake"]), (1, 3))
        assert_greater_than_or_equal(staking["lastsignedslot"], staking["lastslot"])
        assert_greater_than(staking["nextslot"], staking["slot"])
        assert_equal(staking["nexttime"] - staking["nextslot"], schedule["slots"][0]["time"] - schedule["slots"][0]["slot"])
        assert_equal(n0.getvalidator(XONLY[0])["local"], True)
        assert_equal(n0.getvalidator(XONLY[0])["nextslot"], staking["nextslot"])
        assert_equal([v["local"] for v in n0.getvalidators()["validators"] if v["pubkey"] == XONLY[0]], [True])

        self.log.info("getblock: a block of stake, its slot, its proposer (the leader of the slot)")
        tip = n0.getblock(n0.getbestblockhash())
        assert_equal(tip["kind"], "pos")
        epoch_schedule = n0.getleaderschedule(tip["slot"] // 20)
        assert_equal(tip["proposer"], epoch_schedule["slots"][tip["slot"] % 20]["leader"])
        assert_equal(tip["finality"], {"status": "soft", "anchors": 0})

        self.log.info("Staking off: no more blocks of stake; on again: they come")
        for n in self.validators():
            assert_equal(n.setstaking(False), {"enabled": False})
        # (A block under way when staking stopped may still come.)
        slot = n0.getposinfo()["slot"]
        self.wait_until(lambda: n0.getposinfo()["slot"] > slot + 2, timeout=30)
        self.wait_in_sync()
        height = n0.getblockcount()
        produced = [n.getstakinginfo()["produced"] for n in self.validators()]
        slot = n0.getposinfo()["slot"]
        self.wait_until(lambda: n0.getposinfo()["slot"] > slot + 3, timeout=30)
        assert_equal(n0.getblockcount(), height)
        assert_equal([n.getstakinginfo()["produced"] for n in self.validators()], produced)
        assert_equal(n0.getstakinginfo()["enabled"], False)
        for n in self.validators():
            n.setstaking(True)
        self.wait_until(lambda: n0.getblockcount() > height + 2, timeout=60)

        self.log.info("Anchors: getanchorinfo, the acknowledgement in getblock, the anchored and final tips")
        ticket_address = n0.getnewaddress()
        n0.setbmm(True, ticket_address, True, Decimal("0.0001"))
        txid = n0.sendtoaddress(n1.getnewaddress(), 1)
        self.wait_until(lambda: n0.gettransaction(txid)["confirmations"] > 0, timeout=30)
        tx = n0.gettransaction(txid)
        assert_equal(tx["finality"], "soft")
        assert_equal(n0.getblock(tx["blockhash"])["finality"]["status"], "soft")
        balances = n0.getbalances()["mine"]
        assert_greater_than(balances["finality"]["soft"], 0)
        for _ in range(3):
            self.anchor()
        self.wait_in_sync()
        info = n0.getanchorinfo()
        assert_greater_than(info["anchors"], 3)
        assert_equal(info["finalanchors"], 1)
        recent = info["recent"]
        assert_greater_than(len(recent), 3)
        # Newest first: the blocks anchored come lower and lower; the commitments earlier and earlier.
        for newer, older in zip(recent, recent[1:]):
            assert_greater_than(newer["anchoredheight"], older["anchoredheight"])
            assert_greater_than(newer["mainheight"], older["mainheight"])
        acked = [a for a in recent if a["kind"] == "ticket"][0]
        block = n0.getblock(acked["block"])
        assert_equal(block["kind"], "pos")
        assert_equal(block["ack"]["anchored"], acked["anchored"])
        assert_equal(block["ack"]["mainheight"], acked["mainheight"])
        assert_equal(block["nonce"], acked["mainheight"])
        tickets = {t["hash"]: t for t in info["tickets"]}
        assert block["ack"]["ticket"] in tickets
        assert_equal(tickets[block["ack"]["ticket"]]["committedheight"], acked["mainheight"])
        chaininfo = n0.getblockchaininfo()
        assert_equal(chaininfo["anchored_tip"], info["anchoredtip"])
        # On regtest one anchor makes a block final.
        assert_equal(chaininfo["final_tip"], info["anchoredtip"])
        assert_equal(n0.getblockheader(info["anchoredtip"])["height"], info["anchoredheight"])
        assert_greater_than_or_equal(info["pot"]["accrued"], info["pot"]["paid"])
        assert_greater_than_or_equal(info["pot"]["due"], 0)
        assert_equal(len(n0.getanchorinfo(2)["recent"]), 2)
        # Nothing skipped: every commitment is an anchor of the chain.
        assert_equal(info["skipped"], [])
        self.wait_in_sync()
        assert_peg_balance(main, self.validators())
        assert_raises_rpc_error(-8, "count must be", n0.getanchorinfo, 101)

        self.log.info("The wallet: the transaction is final once an anchor covers it")
        self.wait_until(lambda: n0.gettransaction(txid)["finality"] == "final", timeout=60)
        assert_equal(n0.getblock(tx["blockhash"])["finality"], {"status": "final", "anchors": 1})
        listed = [t for t in n0.listtransactions("*", 100) if t["txid"] == txid]
        assert all(t["finality"] == "final" for t in listed)
        mine = n0.getbalances()["mine"]
        assert_greater_than(mine["finality"]["final"], 0)
        assert_equal(mine["finality"]["anchored"], 0)
        assert_equal(mine["finality"]["soft"] + mine["finality"]["final"], mine["trusted"])

        self.log.info("Tickets: a peer asks for the ticket of a commitment, and gets it")
        peer = n0.add_p2p_connection(FastBlocksPeer())
        wanted = block["ack"]["ticket"]
        peer.send_and_ping(MsgGetTickets(ser_compact_size(2) + bytes.fromhex(wanted)[::-1] + bytes(32)))
        self.wait_until(lambda: any(ticket_hash(t) == wanted for t in peer.tickets), timeout=30)
        # More than 16 at once: the peer misbehaves (and is disconnected).
        peer.send_without_ping(MsgGetTickets(ser_compact_size(17) + bytes(32 * 17)))
        peer.wait_for_disconnect()

        self.log.info("Evidence: two blocks a leader signed for one slot")
        header = bytes.fromhex(n0.getblockheader(tip["hash"], False))
        assert_equal(len(header), 176)
        signer = KEYS[XONLY.index(tip["proposer"])]
        rival = bytearray(header)
        rival[36:68] = bytes(range(32))
        sighash = TaggedHash("hpos/block", hash256(bytes(rival[:112])))
        rival[112:176] = sign_schnorr(signer, sighash)
        proof = (header + bytes(rival)).hex()
        result = n1.submitevidence(proof)
        assert_equal(result, {"pubkey": tip["proposer"], "slot": tip["slot"], "new": True})
        assert_equal(n1.submitevidence(proof)["new"], False)
        # Relayed to the others, which check it.
        self.wait_until(lambda: all(len(n.listevidence()) == 1 for n in self.validators()), timeout=30)
        listed = n2.listevidence()[0]
        assert_equal((listed["pubkey"], listed["slot"]), (tip["proposer"], tip["slot"]))
        assert_equal(sorted(listed["hashes"]), sorted([tip["hash"], hash256(bytes(rival[:112]))[::-1].hex()]))
        assert_equal(n2.getvalidator(tip["proposer"])["offences"], 1)
        assert_equal(n0.getposinfo()["evidence"], 1)
        # Refused: one header twice, a signature by another key, bytes that do not read.
        assert_raises_rpc_error(RPC_VERIFY_REJECTED, "bad-pos-evidence-same", n1.submitevidence, (header + header).hex())
        forged = bytearray(rival)
        other = KEYS[(XONLY.index(tip["proposer"]) + 1) % 3]
        forged[112:176] = sign_schnorr(other, sighash)
        assert_raises_rpc_error(RPC_VERIFY_REJECTED, "bad-pos-evidence-sig", n1.submitevidence, (header + bytes(forged)).hex())
        assert_raises_rpc_error(RPC_DESERIALIZATION_ERROR, "does not read", n1.submitevidence, header.hex())

        self.log.info("An observer that joins late takes in the whole chain, and asks for the newest ticket")
        # The validators pause, so that the newest commitment stays unacknowledged.
        for n in self.validators():
            n.setstaking(False)
        self.wait_in_sync()
        self.wait_until(lambda: len(main.getrawmempool()) > 0, timeout=60)
        self.mine_main()
        main_height = main.getblockcount()
        self.wait_until(lambda: any(t.get("committedheight") == main_height for t in n0.getanchorinfo()["tickets"]), timeout=30)
        newest = [t for t in n0.getanchorinfo()["tickets"] if t.get("committedheight") == main_height][0]
        assert_greater_than(main_height, n0.getposinfo()["lastanchormainheight"])
        assert_equal(observer.getblockcount(), 0)
        for i in range(3):
            self.connect_nodes(3, i)
        self.wait_in_sync(self.nodes)
        assert_equal(observer.getanchorinfo()["recent"], n0.getanchorinfo()["recent"])
        # At the next mainchain block it asks its peers for the tickets of the commitments it lacks (if
        # the merged miner did not relay it again meanwhile).
        self.mine_main()
        self.wait_until(lambda: newest["hash"] in [t["hash"] for t in observer.getanchorinfo()["tickets"]], timeout=30)
        # The validators go on, and acknowledge it: the observer follows.
        anchors = n0.getposinfo()["anchors"]
        for n in self.validators():
            n.setstaking(True)
        self.wait_until(lambda: observer.getposinfo()["lastanchormainheight"] >= main_height, timeout=60)
        for n in self.validators():
            n.setstaking(False)
        self.wait_in_sync(self.nodes)
        assert_greater_than(observer.getposinfo()["anchors"], anchors)
        states = [n.getsidechainstate() for n in self.nodes]
        assert_equal(len({(st["bestblock"], st["hash"]) for st in states}), 1)
        assert_equal(observer.getanchorinfo()["recent"], n0.getanchorinfo()["recent"])
        tip_hash = observer.getbestblockhash()
        assert_equal(observer.getblock(tip_hash)["finality"], n0.getblock(tip_hash)["finality"])
        assert_peg_balance(main, self.nodes)

if __name__ == '__main__':
    FastBlocksRpcTest(__file__).main()
