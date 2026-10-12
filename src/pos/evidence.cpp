// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/evidence.h>

#include <pos/header.h>
#include <pos/schedule.h>

#include <algorithm>

namespace pos {

void DoubleProposal::Normalize()
{
    if (b.GetHash() < a.GetHash()) std::swap(a, b);
}

int64_t DoubleProposal::Slot(const Consensus::PosParams& params) const { return SlotOf(a.nTime, params); }

bool CheckEvidenceForm(const DoubleProposal& proof, const Consensus::PosParams& params, std::string& reason)
{
    const auto fail{[&](const char* why) {
        reason = why;
        return false;
    }};
    if (!IsPosHeader(proof.a) || !IsPosHeader(proof.b)) return fail("bad-pos-evidence-kind");
    const int64_t time{proof.a.nTime};
    if (proof.b.nTime != proof.a.nTime || time <= params.genesis_time || (time - params.genesis_time) % params.slot_seconds != 0) {
        return fail("bad-pos-evidence-slot");
    }
    if (proof.a.GetHash() == proof.b.GetHash()) return fail("bad-pos-evidence-same");
    return true;
}

std::optional<Offence> CheckEvidence(const DoubleProposal& proof, const uint256& pubkey, const Consensus::PosParams& params, std::string& reason)
{
    if (!CheckEvidenceForm(proof, params, reason)) return std::nullopt;
    if (!CheckBlockSig(proof.a, pubkey) || !CheckBlockSig(proof.b, pubkey)) {
        reason = "bad-pos-evidence-sig";
        return std::nullopt;
    }
    return Offence{.pubkey = pubkey, .slot = SlotOf(proof.a.nTime, params)};
}

bool EvidencePool::Add(const Offence& offence, DoubleProposal proof)
{
    proof.Normalize();
    LOCK(m_mutex);
    if (!m_proofs.emplace(offence, std::move(proof)).second) return false;
    m_unrelayed.push_back(offence);
    // Bounded: the proofs of the oldest slots go first.
    while (m_proofs.size() > MAX_EVIDENCE) {
        const auto oldest{std::min_element(m_proofs.begin(), m_proofs.end(), [](const auto& x, const auto& y) { return x.first.slot < y.first.slot; })};
        std::erase(m_unrelayed, oldest->first);
        m_proofs.erase(oldest);
    }
    return true;
}

std::optional<DoubleProposal> EvidencePool::NoteSigned(const CBlockHeader& header, int64_t slot)
{
    LOCK(m_mutex);
    const auto [it, inserted]{m_signed.emplace(slot, header)};
    if (inserted) {
        // Bounded: the oldest slots go first (a proof of so old a slot is old news).
        while (m_signed.size() > MAX_SIGNED_HEADERS) m_signed.erase(m_signed.begin());
        return std::nullopt;
    }
    if (it->second.GetHash() == header.GetHash()) return std::nullopt;
    DoubleProposal proof{.a = it->second, .b = header};
    proof.Normalize();
    return proof;
}

std::vector<std::pair<Offence, DoubleProposal>> EvidencePool::All() const
{
    LOCK(m_mutex);
    return {m_proofs.begin(), m_proofs.end()};
}

std::optional<DoubleProposal> EvidencePool::Get(const Offence& offence) const
{
    LOCK(m_mutex);
    const auto found{m_proofs.find(offence)};
    if (found == m_proofs.end()) return std::nullopt;
    return found->second;
}

size_t EvidencePool::Size() const
{
    LOCK(m_mutex);
    return m_proofs.size();
}

std::vector<DoubleProposal> EvidencePool::TakeUnrelayed()
{
    LOCK(m_mutex);
    std::vector<DoubleProposal> proofs;
    for (const Offence& offence : m_unrelayed) {
        if (const auto found{m_proofs.find(offence)}; found != m_proofs.end()) proofs.push_back(found->second);
    }
    m_unrelayed.clear();
    return proofs;
}

void EvidencePool::MarkRelayed(const Offence& offence)
{
    LOCK(m_mutex);
    std::erase(m_unrelayed, offence);
}

} // namespace pos
