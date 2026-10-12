// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POS_EVIDENCE_H
#define BITCOIN_POS_EVIDENCE_H

#include <pos/params.h>
#include <primitives/block.h>
#include <serialize.h>
#include <sync.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pos {

class ValidatorSet;

//! The P2P message that relays proofs that a leader signed two blocks for one slot.
inline constexpr const char* MSG_EVIDENCE{"fbevidence"};
//! Proofs a node keeps, at most: those of the oldest slots go first.
inline constexpr size_t MAX_EVIDENCE{1000};
//! Signed headers a node remembers, one per slot, to find a second one: those of the oldest slots go first.
inline constexpr size_t MAX_SIGNED_HEADERS{4000};

/**
 * Two headers the leader of a slot signed for that slot: the proof that it signed twice, which
 * slashing punishes (doc/fastblocks.md, "Evidence"). The same proof always reads the same: the
 * header with the lower hash first (Normalize).
 */
struct DoubleProposal {
    CBlockHeader a;
    CBlockHeader b;

    SERIALIZE_METHODS(DoubleProposal, obj) { READWRITE(obj.a, obj.b); }

    void Normalize();
    /** The slot of the first header. */
    int64_t Slot(const Consensus::PosParams& params) const;
};

/** What a valid proof says: who signed twice, and at which slot. */
struct Offence {
    uint256 pubkey;
    int64_t slot{0};
    friend auto operator<=>(const Offence&, const Offence&) = default;
};

/**
 * Check a proof against the validator set (context free: the set is the chain's).
 * Reasons: bad-pos-evidence-kind (not two headers of blocks of stake), bad-pos-evidence-slot (not
 * the start of one slot, both), bad-pos-evidence-same (one header twice), bad-pos-evidence-sig (not
 * both signed by the slot's leader).
 * @return the offence; nothing, with `reason` set, if the proof is not one
 */
std::optional<Offence> CheckEvidence(const DoubleProposal& proof, const ValidatorSet& validators, const Consensus::PosParams& params,
                                     const uint256& genesis_hash, std::string& reason);

/**
 * The proofs a node knows, one per offence, and the headers it saw signed, to make proofs of its
 * own. Bounded. Thread safe.
 */
class EvidencePool
{
public:
    /** Keep a proof that was checked. @return whether it was new */
    bool Add(const Offence& offence, DoubleProposal proof) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /**
     * Note a header whose signature by the leader of its slot was checked. If another one of the
     * same slot was noted before, the proof that the leader signed both.
     */
    std::optional<DoubleProposal> NoteSigned(const CBlockHeader& header, int64_t slot) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    std::vector<std::pair<Offence, DoubleProposal>> All() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    std::optional<DoubleProposal> Get(const Offence& offence) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    size_t Size() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** Proofs added since the last call, to relay. */
    std::vector<DoubleProposal> TakeUnrelayed() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** Have the next TakeUnrelayed leave out a proof (it was relayed already). */
    void MarkRelayed(const Offence& offence) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

private:
    mutable Mutex m_mutex;
    std::map<Offence, DoubleProposal> m_proofs GUARDED_BY(m_mutex);
    std::vector<Offence> m_unrelayed GUARDED_BY(m_mutex);
    //! The first header signed for each slot seen.
    std::map<int64_t, CBlockHeader> m_signed GUARDED_BY(m_mutex);
};

} // namespace pos

#endif // BITCOIN_POS_EVIDENCE_H
