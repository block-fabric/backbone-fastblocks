// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_RPC_POS_H
#define BITCOIN_RPC_POS_H

#include <rpc/util.h>

#include <vector>

class CBlock;
class CBlockIndex;
class ChainstateManager;
class UniValue;

/** What getblock adds on FastBlocks (all optional): the kind of block, its slot and proposer, its acknowledgement, its finality. */
std::vector<RPCResult> PosBlockFields();
/** Add them to getblock's result, if the FastBlocks hooks are installed. */
void PushPosBlockInfo(const ChainstateManager& chainman, const CBlock& block, const CBlockIndex& index, UniValue& result);
/** Add what getblockchaininfo adds on FastBlocks (the anchored and the final tip), if the FastBlocks hooks are installed. */
void PushPosChainInfo(const ChainstateManager& chainman, UniValue& result);
/** What getsidechainstate adds on FastBlocks: pegbalance. */
std::vector<RPCResult> PegBalanceFields();
/**
 * Add pegbalance to getsidechainstate's result, if the FastBlocks hooks are installed: the coins the
 * chain holds against the mainchain's escrow, as of the tip (design section 6.3): spendable coins,
 * withdrawals (waiting or in a bundle), the fee pot, and the payouts owed. Without fees left unclaimed
 * or coins burned, their total is the escrow on the mainchain after the block `mainheight`.
 */
void PushPegBalance(ChainstateManager& chainman, UniValue& result);

#endif // BITCOIN_RPC_POS_H
