// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_KERNEL_TESTNET_ACTIVATION_H
#define BITCOIN_KERNEL_TESTNET_ACTIVATION_H

/**
 * FastBlocks on the Chains test network: the height of the Chains testnet block that activated FastBlocks in
 * slot 5 (Consensus::SidechainParams::main_activation_height of the test network).
 *
 * The test network was reset on 17/Oct/2026 (new genesis blocks for Chains and every sidechain, every
 * rule from block 0). Slot 5 activates again on the new Chains testnet; until then this is 0, which
 * the node accepts on a test network with a warning in its log (CheckSidechainActivation) and which
 * leaves nothing of the mainchain out. Once the slot is active, set it to the "activationheight" that
 * `chains-cli -testnet getsidechain 5` shows, rebuild, and start the nodes of the test network
 * only then: a node started before keeps the record of the mainchain it began (from height 0, not from
 * the activation; harmless, but start new datadirs with the height set).
 *
 * FastBlocks' test network restarted on 19/Oct/2026 from a new genesis, every rule from its first block
 * (the fee split and the second fee curve too: no height of the test network to set later); slot 5 stays
 * the one activated at Chains testnet block 69.
 */
inline constexpr int TESTNET_MAIN_ACTIVATION_HEIGHT{69};

#endif // BITCOIN_KERNEL_TESTNET_ACTIVATION_H
