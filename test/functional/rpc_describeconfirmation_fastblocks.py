#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""FastBlocks' own questions through the host over RPC (src/webui/remotehost.h), as the Chains app asks them.

Its pages bond and unbond through host methods (fastblocks.bond, fastblocks.unbond, local.cpp), which ask first: through
webuihost the question comes back, with the validator key and the amount, and nothing runs; a bond changed after its
question (another amount, another key) is refused and the token used up. (The kit's own: rpc_describeconfirmation.py.)
"""

from feature_fastblocks import KEYS, FastBlocksTest
from test_framework.key import compute_xonly_pubkey
from test_framework.util import assert_raises_rpc_error

REFUSED = -32802


class FastBlocksDescribeConfirmationTest(FastBlocksTest):
    def set_test_params(self):
        super().set_test_params()
        self.num_nodes = 1
        self.extra_args = [self.side_args]

    def run_test(self):
        super().run_test()

    def run_sidechain_test(self):
        node = self.nodes[0]
        wallet = node.listwallets()[0]
        pubkey = compute_xonly_pubkey(KEYS[0])[0].hex()
        bond = {"kind": "host", "method": "fastblocks.bond", "params": {"wallet": wallet, "pubkey": pubkey, "amount": "2"}}
        self.log.info("a bond: asked with its key and amount, nothing run")
        ask = node.webuihost(bond)["confirm"]
        assert "bond" in ask["title"].lower(), ask["title"]
        values = [r["value"] for r in ask["rows"]]
        assert pubkey in values, values
        assert any(v.startswith("2.00000000") for v in values), values
        self.log.info("changed after its question: refused, its token used up")
        changed = {"kind": "host", "method": "fastblocks.bond", "params": {"wallet": wallet, "pubkey": pubkey, "amount": "200"}}
        assert_raises_rpc_error(REFUSED, "not the one that was confirmed", node.webuihost, changed, ask["token"])
        assert_raises_rpc_error(REFUSED, "not known", node.webuihost, bond, ask["token"])


if __name__ == "__main__":
    FastBlocksDescribeConfirmationTest(__file__).main()
