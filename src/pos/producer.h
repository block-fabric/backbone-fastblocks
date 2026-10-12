// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POS_PRODUCER_H
#define BITCOIN_POS_PRODUCER_H

#include <kernel/cs_main.h>
#include <key.h>
#include <pos/header.h>
#include <pos/protection.h>
#include <primitives/block.h>
#include <script/script.h>
#include <sync.h>
#include <uint256.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class ChainstateManager;
namespace node {
struct NodeContext;
} // namespace node

namespace pos {

class FastBlocksHooks;

/**
 * The block producer of a validator node: at each slot one of its keys leads, it builds a PoS block
 * on the best chain (with the acknowledgement of an anchor if one is due), has the slashing
 * protection note the slot, signs the block and gives it to the node, which relays it.
 */
class Producer
{
public:
    /**
     * The producer the node's options ask for (-validatorkeyfile, -validatorpayout); none without keys.
     * @return nullptr, with `error` set if the options are wrong
     */
    static std::unique_ptr<Producer> FromArgs(node::NodeContext& node, const FastBlocksHooks& hooks, std::string& error);

    Producer(node::NodeContext& node, const FastBlocksHooks& hooks, std::vector<CKey> keys, CScript payout, std::unique_ptr<SlashingProtection> protection);
    ~Producer();

    void Start() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void Stop() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /**
     * Make the block of `slot`, if a key of this node leads it.
     * @return its hash; nothing if no key of this node leads the slot, or, with `error` set, if no block could be made
     */
    std::optional<uint256> Produce(int64_t slot, std::string& error) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    struct Status {
        uint64_t produced{0};
        uint64_t acknowledgements{0};
        uint64_t failed{0};
        int64_t last_slot{-1};
        std::string error;
    };
    Status GetStatus() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** Whether the producer makes blocks at its slots (setstaking): on when the node starts. */
    bool Enabled() const { return m_enabled.load(); }
    void SetEnabled(bool enabled) { m_enabled = enabled; }
    /** The highest slot this node signed, by the slashing protection; -1 if none. */
    int64_t LastSignedSlot() const;
    /** The x-only keys of this node's validators. */
    std::vector<uint256> Keys() const;
    /** The proof of possession of one of this node's validator keys for a bond owned by `owner`; nothing if the key is not this node's. */
    std::optional<std::array<unsigned char, 64>> SignBondPop(const uint256& pubkey, const uint160& owner) const;
    const CScript& Payout() const { return m_payout; }

    /**
     * Whether the node is behind, and should not make blocks: it has a block of more work than its
     * tip, and all below it, and has yet to connect it. A header of more work alone is not: its block
     * may never come (a leader can sign a header that claims an acknowledgement, and keep the block).
     */
    static bool Behind(ChainstateManager& chainman) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /** The producer's share of fees `fees`, in integers: floor(fees * bps / 10000). */
    static int64_t ProducerShare(int64_t fees, int64_t bps);
    /** Build the block of `slot` on the tip, with `ack` or not, and the proofs `evidence`, the fees split; not signed. */
    std::optional<CBlock> Build(int64_t slot, const std::optional<AnchorAck>& ack, std::string& error, const std::vector<DoubleProposal>& evidence = {}) const;
    /** Sign a block with `key` (no slashing protection: for the producer, and tests). */
    static bool Sign(CBlock& block, const CKey& key);

private:
    void Run() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    node::NodeContext& m_node;
    const FastBlocksHooks& m_hooks;
    const std::vector<CKey> m_keys;
    const CScript m_payout;
    const std::unique_ptr<SlashingProtection> m_protection;

    mutable Mutex m_mutex;
    std::condition_variable_any m_wake;
    std::thread m_thread GUARDED_BY(m_mutex);
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_enabled{true};
    Status m_status GUARDED_BY(m_mutex);
    //! Serializes Produce: the thread and RPC.
    Mutex m_produce_mutex;
};

} // namespace pos

#endif // BITCOIN_POS_PRODUCER_H
