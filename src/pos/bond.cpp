// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/bond.h>

#include <crypto/common.h>
#include <hash.h>
#include <pubkey.h>
#include <tinyformat.h>
#include <util/strencodings.h>

#include <algorithm>

namespace pos {
namespace {
/** If `script` is OP_RETURN and one push that starts with `tag`, what follows the tag. */
std::optional<std::vector<unsigned char>> ParseTagged(const CScript& script, std::span<const unsigned char> tag)
{
    CScript::const_iterator pc{script.begin()};
    opcodetype opcode;
    std::vector<unsigned char> data;
    if (!script.GetOp(pc, opcode) || opcode != OP_RETURN) return std::nullopt;
    if (!script.GetOp(pc, opcode, data) || opcode > OP_PUSHDATA4 || pc != script.end()) return std::nullopt;
    if (data.size() < tag.size() || !std::equal(tag.begin(), tag.end(), data.begin())) return std::nullopt;
    data.erase(data.begin(), data.begin() + tag.size());
    return data;
}

CScript Tagged(std::span<const unsigned char> tag, std::span<const unsigned char> payload)
{
    std::vector<unsigned char> data{tag.begin(), tag.end()};
    data.insert(data.end(), payload.begin(), payload.end());
    return CScript() << OP_RETURN << data;
}

} // namespace

CScript BondScript(const uint256& pubkey, const uint160& owner, const std::array<unsigned char, 64>& pop, std::span<const unsigned char> owner_sig)
{
    std::vector<unsigned char> payload{pubkey.begin(), pubkey.end()};
    payload.insert(payload.end(), owner.begin(), owner.end());
    payload.insert(payload.end(), pop.begin(), pop.end());
    payload.insert(payload.end(), owner_sig.begin(), owner_sig.end());
    return Tagged(BOND_TAG, payload);
}

CScript UnbondScript(const Unbond& unbond)
{
    std::vector<unsigned char> payload{unbond.pubkey.begin(), unbond.pubkey.end()};
    payload.resize(32 + 12);
    WriteLE64(payload.data() + 32, static_cast<uint64_t>(unbond.amount));
    WriteLE32(payload.data() + 40, unbond.sequence);
    payload.insert(payload.end(), unbond.signature.begin(), unbond.signature.end());
    return Tagged(UNBOND_TAG, payload);
}

bool IsBondScript(const CScript& script) { return ParseTagged(script, BOND_TAG).has_value(); }
bool IsUnbondScript(const CScript& script) { return ParseTagged(script, UNBOND_TAG).has_value(); }

std::optional<Bond> ParseBond(const CTxOut& out)
{
    const auto payload{ParseTagged(out.scriptPubKey, BOND_TAG)};
    constexpr size_t BASE{32 + 20 + 64};
    if (!payload || (payload->size() != BASE && payload->size() != BASE + CPubKey::COMPACT_SIGNATURE_SIZE)) return std::nullopt;
    Bond bond;
    bond.pubkey = uint256{std::span{*payload}.first(32)};
    std::copy(payload->begin() + 32, payload->begin() + 52, bond.owner.begin());
    std::copy(payload->begin() + 52, payload->begin() + BASE, bond.pop.begin());
    bond.owner_sig.assign(payload->begin() + BASE, payload->end());
    bond.amount = out.nValue;
    return bond;
}

std::optional<Unbond> ParseUnbond(const CTxOut& out)
{
    const auto payload{ParseTagged(out.scriptPubKey, UNBOND_TAG)};
    if (!payload || payload->size() != 32 + 12 + CPubKey::COMPACT_SIGNATURE_SIZE) return std::nullopt;
    Unbond unbond;
    unbond.pubkey = uint256{std::span{*payload}.first(32)};
    const uint64_t amount{ReadLE64(payload->data() + 32)};
    if (amount > static_cast<uint64_t>(MAX_MONEY)) return std::nullopt;
    unbond.amount = static_cast<CAmount>(amount);
    unbond.sequence = ReadLE32(payload->data() + 40);
    unbond.signature.assign(payload->begin() + 44, payload->end());
    return unbond;
}

uint256 BondPopHash(const uint160& owner, const uint256& genesis_hash)
{
    return (TaggedHash("hpos/bond") << owner << genesis_hash).GetSHA256();
}

bool CheckBondPop(const uint256& pubkey, const uint160& owner, const std::array<unsigned char, 64>& pop, const uint256& genesis_hash)
{
    const XOnlyPubKey key{std::span<const unsigned char>{pubkey.begin(), pubkey.end()}};
    if (!key.IsFullyValid()) return false;
    return key.VerifySchnorr(BondPopHash(owner, genesis_hash), pop);
}

std::string UnbondMessage(const uint256& pubkey, CAmount amount, uint32_t sequence, const uint256& genesis_hash)
{
    const std::string what{amount == UNBOND_ALL ? std::string{"all"} : strprintf("%d", amount)};
    return strprintf("FastBlocks unbond %s %s %u %s", HexStr(pubkey), what, sequence, genesis_hash.GetHex());
}

std::string BondMessage(const uint256& pubkey, CAmount amount, const COutPoint& spent, const uint256& genesis_hash)
{
    return strprintf("FastBlocks bond %s %d %s:%u %s", HexStr(pubkey), amount, spent.hash.GetHex(), spent.n, genesis_hash.GetHex());
}

} // namespace pos
