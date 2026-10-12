// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/header.h>

#include <arith_uint256.h>
#include <hash.h>
#include <pubkey.h>
#include <script/solver.h>
#include <streams.h>

#include <ios>

namespace pos {

bool IsPosHeader(const CBlockHeader& header) { return header.IsExtended(); }

int64_t SlotOf(int64_t time, const Consensus::PosParams& params)
{
    if (time <= params.genesis_time) return 0;
    return (time - params.genesis_time) / params.slot_seconds;
}

int64_t SlotTime(int64_t slot, const Consensus::PosParams& params) { return params.genesis_time + slot * params.slot_seconds; }

int64_t EpochOf(int64_t slot, const Consensus::PosParams& params) { return slot / params.slots_per_epoch; }

uint256 AnchorTicket::Hash() const
{
    HashWriter hasher{TaggedHash("hpos/ticket")};
    hasher << *this;
    return hasher.GetSHA256();
}

namespace {
//! First byte of non-empty sections: whether an acknowledgement follows. The proofs come after.
constexpr uint8_t SECTION_NO_ACK{0};
constexpr uint8_t SECTION_ACK{1};
} // namespace

std::vector<unsigned char> PosSections::Encode() const
{
    if (!ack && evidence.empty()) return {};
    DataStream s{};
    s << (ack ? SECTION_ACK : SECTION_NO_ACK);
    if (ack) s << *ack;
    s << evidence;
    return {UCharCast(s.data()), UCharCast(s.data()) + s.size()};
}

bool PosSections::Decode(std::span<const unsigned char> bytes, PosSections& sections)
{
    sections = {};
    if (bytes.empty()) return true;
    try {
        SpanReader s{bytes};
        uint8_t kind;
        s >> kind;
        if (kind != SECTION_ACK && kind != SECTION_NO_ACK) return false;
        PosSections read;
        if (kind == SECTION_ACK) s >> read.ack.emplace();
        const uint64_t count{ReadCompactSize(s)};
        if (count > MAX_SECTION_EVIDENCE) return false;
        read.evidence.resize(count);
        for (DoubleProposal& proof : read.evidence) s >> proof;
        if (!s.empty()) return false;
        // Sections with nothing in them are no bytes at all, so that the header commits to null.
        if (!read.ack && read.evidence.empty()) return false;
        sections = std::move(read);
        return true;
    } catch (const std::ios_base::failure&) {
        return false;
    }
}

uint256 SectionsHash(std::span<const unsigned char> sections)
{
    return (TaggedHash("hpos/sections") << sections).GetSHA256();
}

uint256 BlockSigHash(const CBlockHeader& header)
{
    return (TaggedHash("hpos/block") << header.GetHash()).GetSHA256();
}

bool CheckBlockSig(const CBlockHeader& header, const uint256& xonly_pubkey)
{
    const XOnlyPubKey key{std::span<const unsigned char>{xonly_pubkey.begin(), xonly_pubkey.end()}};
    if (!key.IsFullyValid()) return false;
    return key.VerifySchnorr(BlockSigHash(header), header.ext.sig);
}

CScript PotScript()
{
    return CScript() << OP_RETURN << std::vector<unsigned char>(std::begin(POT_TAG), std::end(POT_TAG));
}

bool IsPotScript(const CScript& script) { return script == PotScript(); }

bool IsValidPayee(const CScript& payee)
{
    if (payee.size() > MAX_PAYEE_SIZE) return false;
    std::vector<std::vector<unsigned char>> solutions;
    const TxoutType type{Solver(payee, solutions)};
    return type != TxoutType::NONSTANDARD && type != TxoutType::NULL_DATA;
}

uint32_t PosBits(const uint256& pow_limit) { return UintToArith256(pow_limit).GetCompact(); }

} // namespace pos
