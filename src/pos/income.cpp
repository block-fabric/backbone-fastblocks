// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/income.h>

#include <pos/evidence.h>
#include <pos/header.h>
#include <primitives/block.h>

#include <map>

namespace pos {

bool IsAnchorBlock(const CBlock& block)
{
    if (!IsPosHeader(block)) return true;
    PosSections sections;
    return PosSections::Decode(block.vSections, sections) && sections.ack.has_value();
}

std::optional<CScript> AnchorPayee(const CBlock& block)
{
    if (block.vtx.empty() || block.vtx[0]->vout.empty()) return std::nullopt;
    if (!IsPosHeader(block)) return block.vtx[0]->vout[0].scriptPubKey;
    PosSections sections;
    if (!PosSections::Decode(block.vSections, sections) || !sections.ack) return std::nullopt;
    return sections.ack->ticket.payee;
}

std::vector<CScript> ReporterScripts(const CBlock& block, const std::optional<uint256>& proposer, const uint256& genesis)
{
    std::vector<CScript> scripts;
    if (block.vtx.empty() || block.vtx[0]->vout.empty()) return scripts;
    const CScript& reporter{block.vtx[0]->vout[0].scriptPubKey};
    if (IsPosHeader(block)) {
        PosSections sections;
        if (!PosSections::Decode(block.vSections, sections)) return scripts;
        for (const DoubleProposal& proof : sections.evidence) {
            // A leader carrying a proof against itself is paid nothing for it.
            if (proposer && CheckBlockSig(proof.a, *proposer, genesis)) continue;
            scripts.push_back(reporter);
        }
    } else {
        for (const CTxOut& out : block.vtx[0]->vout) {
            if (IsEvidenceScript(out.scriptPubKey)) scripts.push_back(reporter);
        }
    }
    return scripts;
}

std::vector<CoinbaseRole> CoinbaseRoles(const CBlock& block, const std::vector<CScript>& reporters)
{
    if (block.vtx.empty()) return {};
    const std::vector<CTxOut>& outs{block.vtx[0]->vout};
    std::vector<CoinbaseRole> roles(outs.size(), CoinbaseRole::OTHER);
    if (outs.empty()) return roles;
    if (IsPosHeader(block)) roles[0] = CoinbaseRole::PRODUCER;
    const auto payee{AnchorPayee(block)};
    if (!payee || !IsAnchorBlock(block)) return roles;
    // A payout: after the first output, with a value, to a script that can be spent.
    const auto payout{[&](size_t i) { return i > 0 && outs[i].nValue > 0 && !outs[i].scriptPubKey.IsUnspendable(); }};
    // The rewards: the last payouts to each reporter's script, as many as it is owed.
    std::map<CScript, size_t> owed;
    for (const CScript& script : reporters) ++owed[script];
    for (size_t i{outs.size()}; i-- > 1;) {
        if (!payout(i)) continue;
        const auto found{owed.find(outs[i].scriptPubKey)};
        if (found == owed.end() || found->second == 0) continue;
        --found->second;
        roles[i] = CoinbaseRole::REPORTER;
    }
    // The pot: the last other payout to the payee before the rewards (it comes before them).
    size_t end{outs.size()};
    for (size_t i{1}; i < outs.size(); ++i) {
        if (roles[i] == CoinbaseRole::REPORTER) {
            end = i;
            break;
        }
    }
    for (size_t i{end}; i-- > 1;) {
        if (payout(i) && outs[i].scriptPubKey == *payee) {
            roles[i] = CoinbaseRole::ANCHOR;
            break;
        }
    }
    return roles;
}

} // namespace pos
