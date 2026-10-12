// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/staking.h>

#include <addresstype.h>
#include <common/signmessage.h>
#include <crypto/common.h>
#include <hash.h>
#include <pos/header.h>
#include <pubkey.h>
#include <tinyformat.h>
#include <util/strencodings.h>

#include <algorithm>
#include <limits>

namespace pos {
namespace detail {
struct UnbondKey {
    uint64_t release_anchor{0};
    uint256 pubkey;
    uint32_t sequence{0};
};
struct EligibleKey {
    //! UINT64_MAX less the weight: the largest first.
    uint64_t inverse_weight{0};
    uint256 pubkey;
};
struct LivenessKey {
    uint64_t epoch{0};
    uint256 pubkey;
};
struct OffenceKey {
    uint256 pubkey;
    uint64_t slot{0};
};
} // namespace detail
} // namespace pos

namespace sidechain {
template <>
struct KeyCodec<pos::detail::UnbondKey> {
    static void Encode(StoreBytes& out, const pos::detail::UnbondKey& v)
    {
        KeyCodec<uint64_t>::Encode(out, v.release_anchor);
        KeyCodec<uint256>::Encode(out, v.pubkey);
        KeyCodec<uint32_t>::Encode(out, v.sequence);
    }
    static pos::detail::UnbondKey Decode(std::span<const unsigned char>& in)
    {
        pos::detail::UnbondKey v;
        v.release_anchor = KeyCodec<uint64_t>::Decode(in);
        v.pubkey = KeyCodec<uint256>::Decode(in);
        v.sequence = KeyCodec<uint32_t>::Decode(in);
        return v;
    }
};
template <>
struct KeyCodec<pos::detail::EligibleKey> {
    static void Encode(StoreBytes& out, const pos::detail::EligibleKey& v)
    {
        KeyCodec<uint64_t>::Encode(out, v.inverse_weight);
        KeyCodec<uint256>::Encode(out, v.pubkey);
    }
    static pos::detail::EligibleKey Decode(std::span<const unsigned char>& in)
    {
        pos::detail::EligibleKey v;
        v.inverse_weight = KeyCodec<uint64_t>::Decode(in);
        v.pubkey = KeyCodec<uint256>::Decode(in);
        return v;
    }
};
template <>
struct KeyCodec<pos::detail::LivenessKey> {
    static void Encode(StoreBytes& out, const pos::detail::LivenessKey& v)
    {
        KeyCodec<uint64_t>::Encode(out, v.epoch);
        KeyCodec<uint256>::Encode(out, v.pubkey);
    }
    static pos::detail::LivenessKey Decode(std::span<const unsigned char>& in)
    {
        pos::detail::LivenessKey v;
        v.epoch = KeyCodec<uint64_t>::Decode(in);
        v.pubkey = KeyCodec<uint256>::Decode(in);
        return v;
    }
};
template <>
struct KeyCodec<pos::detail::OffenceKey> {
    static void Encode(StoreBytes& out, const pos::detail::OffenceKey& v)
    {
        KeyCodec<uint256>::Encode(out, v.pubkey);
        KeyCodec<uint64_t>::Encode(out, v.slot);
    }
    static pos::detail::OffenceKey Decode(std::span<const unsigned char>& in)
    {
        pos::detail::OffenceKey v;
        v.pubkey = KeyCodec<uint256>::Decode(in);
        v.slot = KeyCodec<uint64_t>::Decode(in);
        return v;
    }
};
} // namespace sidechain

namespace pos {
namespace {
using detail::EligibleKey;
using detail::LivenessKey;
using detail::OffenceKey;
using detail::UnbondKey;

const sidechain::Cell<StakeState> STAKE_STATE{0x50, StakeState{}};
//! (pubkey, slot) of the offences punished: none is punished twice.
const sidechain::Table<OffenceKey, uint8_t> OFFENCES{0x53};
//! Rewards for proofs, owed and not paid yet, in order.
const sidechain::Table<uint64_t, CTxOut> REWARDS{0x54};
const sidechain::Table<uint256, ValidatorRecord> VALIDATORS{0x57};
//! Unbonded coins, by the anchor they are paid at.
const sidechain::Table<UnbondKey, CAmount> UNBOND_QUEUE{0x58};
//! The validators that may be in a set (not banned, enough stake), the largest weight first.
const sidechain::Table<EligibleKey, uint8_t> ELIGIBLE{0x59};
//! Validator sets, by version.
const sidechain::Table<uint64_t, std::vector<Consensus::PosValidator>> SETS{0x5a};
//! The set each anchor (the block that took the peg actions) made, and the anchors in their order.
const sidechain::Table<uint256, AnchorSet> ANCHOR_SETS{0x5b};
const sidechain::Table<uint64_t, uint256> ANCHOR_ORDER{0x5c};
const sidechain::Table<LivenessKey, Liveness> LIVENESS{0x5d};

//! Anchors pruned from ANCHOR_SETS per peg step, at most.
constexpr int MAX_PRUNED_PER_STEP{8};

EligibleKey IndexKey(const uint256& pubkey, const ValidatorRecord& record)
{
    return EligibleKey{.inverse_weight = std::numeric_limits<uint64_t>::max() - static_cast<uint64_t>(record.Weight()), .pubkey = pubkey};
}

bool Fail(std::string& reject_reason, const char* reason)
{
    reject_reason = reason;
    return false;
}

CScript OwnerScript(const uint160& owner) { return GetScriptForDestination(WitnessV0KeyHash{owner}); }
} // namespace

CAmount ValidatorRecord::Unbonding() const
{
    CAmount total{0};
    for (const UnbondingEntry& entry : unbonding) total += entry.amount;
    return total;
}

StakeState GetStakeState(const sidechain::StoreView& view) { return STAKE_STATE.Get(view); }

std::optional<ValidatorRecord> GetValidator(const sidechain::StoreView& view, const uint256& pubkey) { return VALIDATORS.Get(view, pubkey); }

void ForEachValidator(const sidechain::StoreView& view, const std::function<bool(const uint256&, const ValidatorRecord&)>& fn) { VALIDATORS.ForEach(view, fn); }

std::optional<AnchorSet> GetAnchorSet(const sidechain::StoreView& view, const uint256& anchor) { return ANCHOR_SETS.Get(view, anchor); }

std::optional<std::vector<Consensus::PosValidator>> GetSetVersion(const sidechain::StoreView& view, uint64_t version) { return SETS.Get(view, version); }

bool OffencePunished(const sidechain::StoreView& view, const uint256& pubkey, int64_t slot)
{
    return OFFENCES.Contains(view, OffenceKey{.pubkey = pubkey, .slot = static_cast<uint64_t>(slot)});
}

std::optional<Liveness> GetLiveness(const sidechain::StoreView& view, int64_t epoch, const uint256& pubkey)
{
    return LIVENESS.Get(view, LivenessKey{.epoch = static_cast<uint64_t>(epoch), .pubkey = pubkey});
}

void ForEachUnbonding(const sidechain::StoreView& view, const std::function<bool(uint64_t, const uint256&, CAmount)>& fn)
{
    UNBOND_QUEUE.ForEach(view, [&](const UnbondKey& key, const CAmount& amount) { return fn(key.release_anchor, key.pubkey, amount); });
}

CAmount Staking::Penalty(CAmount stake) const
{
    // stake * slash_bps / 10000, by parts: stake * slash_bps may not fit in 64 bits.
    return stake / 10000 * m_params.slash_bps + stake % 10000 * m_params.slash_bps / 10000;
}

void Staking::PutValidator(sidechain::StoreOverlay& out, const uint256& pubkey, const std::optional<ValidatorRecord>& before, const ValidatorRecord& after) const
{
    // The index of the validators that may be in a set follows the record.
    if (before && before->Eligible(m_params)) ELIGIBLE.Erase(out, IndexKey(pubkey, *before));
    if (after.Eligible(m_params)) ELIGIBLE.Put(out, IndexKey(pubkey, after), 1);
    VALIDATORS.Put(out, pubkey, after);
}

void Staking::LoadGenesis(sidechain::StoreOverlay& out) const
{
    StakeState stake{STAKE_STATE.Get(out)};
    if (stake.genesis_loaded) return;
    stake.genesis_loaded = true;
    // As the set of the parameters takes them: a key given twice, with the stake given first.
    const ValidatorSet genesis{m_params};
    for (const Consensus::PosValidator& validator : genesis.Validators()) {
        const auto before{VALIDATORS.Get(out, validator.pubkey)};
        ValidatorRecord after{before.value_or(ValidatorRecord{})};
        after.genesis_stake = validator.stake;
        PutValidator(out, validator.pubkey, before, after);
    }
    STAKE_STATE.Put(out, stake);
}

bool Staking::ApplyTx(sidechain::StoreOverlay& out, const CTransaction& tx, int height, uint64_t anchors, std::string& reject_reason) const
{
    std::optional<StakeState> stake;
    for (const CTxOut& txout : tx.vout) {
        if (IsBondScript(txout.scriptPubKey)) {
            const auto bond{ParseBond(txout)};
            if (!bond) return Fail(reject_reason, "bad-pos-bond");
            if (bond->amount <= 0 || !MoneyRange(bond->amount)) return Fail(reject_reason, "bad-pos-bond-amount");
            if (bond->owner.IsNull()) return Fail(reject_reason, "bad-pos-bond-owner");
            if (!CheckBondPop(bond->pubkey, bond->owner, bond->pop, m_genesis)) return Fail(reject_reason, "bad-pos-bond-pop");
            const auto before{VALIDATORS.Get(out, bond->pubkey)};
            ValidatorRecord after{before.value_or(ValidatorRecord{})};
            if (after.tombstoned) return Fail(reject_reason, "bad-pos-bond-tombstoned");
            // The first bond names the owner; more bonds must name the same.
            if (!after.owner.IsNull() && after.owner != bond->owner) return Fail(reject_reason, "bad-pos-bond-owner");
            after.owner = bond->owner;
            after.bonded += bond->amount;
            if (!MoneyRange(after.bonded)) return Fail(reject_reason, "bad-pos-bond-amount");
            if (after.first_bond_height < 0) after.first_bond_height = height;
            PutValidator(out, bond->pubkey, before, after);
            if (!stake) stake = STAKE_STATE.Get(out);
            stake->bonded += bond->amount;
        } else if (IsUnbondScript(txout.scriptPubKey)) {
            const auto unbond{ParseUnbond(txout)};
            if (!unbond) return Fail(reject_reason, "bad-pos-unbond");
            if (txout.nValue != 0) return Fail(reject_reason, "bad-pos-unbond-value");
            const auto before{VALIDATORS.Get(out, unbond->pubkey)};
            if (!before || before->owner.IsNull()) return Fail(reject_reason, "bad-pos-unbond-unknown");
            if (unbond->sequence != before->unbond_seq) return Fail(reject_reason, "bad-pos-unbond-sequence");
            if (unbond->amount <= 0 || unbond->amount > before->bonded) return Fail(reject_reason, "bad-pos-unbond-amount");
            // All of it, or so that what stays is enough to take part.
            const CAmount remainder{before->bonded - unbond->amount};
            if (remainder != 0 && remainder < m_params.min_stake) return Fail(reject_reason, "bad-pos-unbond-remainder");
            CPubKey signer;
            if (!signer.RecoverCompact(MessageHash(UnbondMessage(unbond->pubkey, unbond->amount, unbond->sequence, m_genesis)), unbond->signature) ||
                !signer.IsCompressed() || signer.GetID() != CKeyID{before->owner}) {
                return Fail(reject_reason, "bad-pos-unbond-sig");
            }
            ValidatorRecord after{*before};
            after.bonded = remainder;
            ++after.unbond_seq;
            const UnbondingEntry entry{.release_anchor = anchors + static_cast<uint64_t>(m_params.unbonding_anchors), .sequence = unbond->sequence, .amount = unbond->amount};
            after.unbonding.push_back(entry);
            UNBOND_QUEUE.Put(out, UnbondKey{.release_anchor = entry.release_anchor, .pubkey = unbond->pubkey, .sequence = entry.sequence}, entry.amount);
            PutValidator(out, unbond->pubkey, before, after);
            if (!stake) stake = STAKE_STATE.Get(out);
            stake->bonded -= unbond->amount;
            stake->unbonding += unbond->amount;
        }
    }
    if (stake) STAKE_STATE.Put(out, *stake);
    return true;
}

ValidatorSet Staking::MakeSet(const sidechain::StoreView& view, int64_t epoch) const
{
    std::vector<Consensus::PosValidator> chosen;
    if (m_params.max_validators == 0) return ValidatorSet{chosen, m_params};
    ELIGIBLE.ForEach(view, [&](const EligibleKey& key, const uint8_t&) {
        const auto record{VALIDATORS.Get(view, key.pubkey)};
        if (!record || record->jailed_until_epoch > epoch) return true;
        chosen.push_back({.pubkey = key.pubkey, .stake = record->Weight()});
        return chosen.size() < m_params.max_validators;
    });
    return ValidatorSet{std::move(chosen), m_params};
}

Staking::PegStepResult Staking::PegStep(sidechain::StoreOverlay& out, const uint256& anchor, uint64_t anchors, int64_t slot, int64_t epoch, std::vector<CTxOut>& owed_tx) const
{
    LoadGenesis(out);
    StakeState stake{STAKE_STATE.Get(out)};

    // Unbonded coins due: to their owner.
    std::vector<std::pair<UnbondKey, CAmount>> due;
    UNBOND_QUEUE.ForEach(out, [&](const UnbondKey& key, const CAmount& amount) {
        if (key.release_anchor > anchors) return false;
        due.emplace_back(key, amount);
        return true;
    });
    for (const auto& [key, amount] : due) {
        UNBOND_QUEUE.Erase(out, key);
        const auto before{VALIDATORS.Get(out, key.pubkey)};
        if (!before) continue; // not reached: an unbond has its validator
        ValidatorRecord after{*before};
        std::erase_if(after.unbonding, [&](const UnbondingEntry& entry) { return entry.sequence == key.sequence && entry.release_anchor == key.release_anchor; });
        PutValidator(out, key.pubkey, before, after);
        owed_tx.emplace_back(amount, OwnerScript(before->owner));
        stake.unbonding -= amount;
    }

    // Rewards for proofs.
    std::vector<uint64_t> paid;
    REWARDS.ForEach(out, [&](const uint64_t& index, const CTxOut& reward) {
        owed_tx.push_back(reward);
        stake.rewards_owed -= reward.nValue;
        paid.push_back(index);
        return true;
    });
    for (const uint64_t index : paid) REWARDS.Erase(out, index);
    if (!paid.empty()) stake.next_reward = 0;

    // A share of the reserve, with the pot (rounded up, so that it empties).
    CAmount release{0};
    if (stake.pot_reserve > 0) {
        const CAmount divisor{std::max<int64_t>(m_params.pot_reserve_anchors, 1)};
        release = stake.pot_reserve / divisor + (stake.pot_reserve % divisor != 0);
        stake.pot_reserve -= release;
    }

    // Validators that made too few of the blocks they led in the epochs before this one are jailed.
    std::vector<std::pair<LivenessKey, Liveness>> counted;
    LIVENESS.ForEach(out, [&](const LivenessKey& key, const Liveness& liveness) {
        if (static_cast<int64_t>(key.epoch) >= epoch) return false;
        counted.emplace_back(key, liveness);
        return true;
    });
    for (const auto& [key, liveness] : counted) {
        LIVENESS.Erase(out, key);
        if (liveness.assigned < m_params.liveness_min_assigned) continue;
        if (int64_t{liveness.produced} * 10000 >= int64_t{liveness.assigned} * m_params.liveness_min_bps) continue;
        const auto before{VALIDATORS.Get(out, key.pubkey)};
        if (!before) continue;
        ValidatorRecord after{*before};
        after.jailed_until_epoch = std::max(after.jailed_until_epoch, epoch + 1 + m_params.jail_epochs);
        PutValidator(out, key.pubkey, before, after);
    }

    // The validator set this anchor makes: a new version if it changed.
    const ValidatorSet set{MakeSet(out, epoch)};
    const auto current{SETS.Get(out, stake.set_version)};
    if (!current || *current != set.Validators()) {
        ++stake.set_version;
        SETS.Put(out, stake.set_version, set.Validators());
    }
    ANCHOR_SETS.Put(out, anchor, AnchorSet{.version = stake.set_version, .anchors = anchors, .slot = slot});
    ANCHOR_ORDER.Put(out, anchors, anchor);

    // Sets no block to come can need: of an anchor older than the window for proofs, followed by one
    // that is older than the epoch before this one (which fixes the sets from this epoch on).
    const int64_t boundary{(epoch - 1) * m_params.slots_per_epoch};
    for (int pruned{0}; pruned < MAX_PRUNED_PER_STEP; ++pruned) {
        std::vector<std::pair<uint64_t, uint256>> oldest;
        ANCHOR_ORDER.ForEach(out, [&](const uint64_t& count, const uint256& hash) {
            oldest.emplace_back(count, hash);
            return oldest.size() < 2;
        });
        if (oldest.size() < 2) break;
        if (oldest[0].first + static_cast<uint64_t>(m_params.evidence_window_anchors) >= anchors) break;
        const auto next{ANCHOR_SETS.Get(out, oldest[1].second)};
        if (!next || next->slot >= boundary) break;
        ANCHOR_ORDER.Erase(out, oldest[0].first);
        ANCHOR_SETS.Erase(out, oldest[0].second);
        std::vector<uint64_t> versions;
        SETS.ForEach(out, [&](const uint64_t& version, const std::vector<Consensus::PosValidator>&) {
            if (version >= next->version) return false;
            versions.push_back(version);
            return true;
        });
        for (const uint64_t version : versions) SETS.Erase(out, version);
    }

    STAKE_STATE.Put(out, stake);
    return PegStepResult{.release = release, .set_active = set.Active()};
}

void Staking::NoteLiveness(sidechain::StoreOverlay& out, int64_t slot, const uint256& producer, const std::vector<std::pair<int64_t, uint256>>& missed) const
{
    const auto add{[&](int64_t at, const uint256& pubkey, bool made) {
        const LivenessKey key{.epoch = static_cast<uint64_t>(EpochOf(at, m_params)), .pubkey = pubkey};
        Liveness liveness{LIVENESS.Get(out, key).value_or(Liveness{})};
        ++liveness.assigned;
        liveness.produced += made;
        LIVENESS.Put(out, key, liveness);
    }};
    for (const auto& [at, pubkey] : missed) add(at, pubkey, false);
    add(slot, producer, true);
}

bool Staking::Punish(sidechain::StoreOverlay& out, const Offence& offence, const std::optional<CScript>& reporter, uint64_t anchors, std::string& reject_reason) const
{
    const OffenceKey offence_key{.pubkey = offence.pubkey, .slot = static_cast<uint64_t>(offence.slot)};
    if (OFFENCES.Contains(out, offence_key)) return Fail(reject_reason, "bad-pos-evidence-known");
    LoadGenesis(out);
    StakeState stake{STAKE_STATE.Get(out)};
    const auto before{VALIDATORS.Get(out, offence.pubkey)};
    // Slashed once: a banned validator's further offences take nothing more (and are no proof a block carries).
    if (before && before->tombstoned) return Fail(reject_reason, "bad-pos-evidence-banned");
    ValidatorRecord after{before.value_or(ValidatorRecord{})};

    // slash_bps of all its stake: bonded first, then what it is unbonding, the latest first.
    const CAmount penalty{Penalty(after.bonded + after.Unbonding())};
    CAmount left{penalty};
    const CAmount from_bond{std::min(left, after.bonded)};
    after.bonded -= from_bond;
    stake.bonded -= from_bond;
    left -= from_bond;
    for (auto it{after.unbonding.rbegin()}; it != after.unbonding.rend() && left > 0; ++it) {
        const CAmount take{std::min(left, it->amount)};
        it->amount -= take;
        left -= take;
        stake.unbonding -= take;
        const UnbondKey key{.release_anchor = it->release_anchor, .pubkey = offence.pubkey, .sequence = it->sequence};
        if (it->amount == 0) {
            UNBOND_QUEUE.Erase(out, key);
        } else {
            UNBOND_QUEUE.Put(out, key, it->amount);
        }
    }
    std::erase_if(after.unbonding, [](const UnbondingEntry& entry) { return entry.amount == 0; });

    // The block that carried the proof is paid its share at the next peg step; the rest goes to the
    // reserve. The offender carrying it itself is paid nothing: all of it to the reserve.
    const CAmount reward{reporter && m_params.reporter_share_divisor > 0 ? penalty / m_params.reporter_share_divisor : 0};
    if (reward > 0) {
        REWARDS.Put(out, stake.next_reward++, CTxOut{reward, *reporter});
        stake.rewards_owed += reward;
    }
    stake.pot_reserve += penalty - reward;
    stake.slashed += penalty;

    // Banned for good: the rest of the bond is unbonded.
    if (after.bonded > 0) {
        const UnbondingEntry entry{.release_anchor = anchors + static_cast<uint64_t>(m_params.unbonding_anchors), .sequence = after.unbond_seq++, .amount = after.bonded};
        after.unbonding.push_back(entry);
        UNBOND_QUEUE.Put(out, UnbondKey{.release_anchor = entry.release_anchor, .pubkey = offence.pubkey, .sequence = entry.sequence}, entry.amount);
        stake.bonded -= after.bonded;
        stake.unbonding += after.bonded;
        after.bonded = 0;
    }
    after.tombstoned = true;
    PutValidator(out, offence.pubkey, before, after);
    OFFENCES.Put(out, offence_key, 1);
    STAKE_STATE.Put(out, stake);
    return true;
}

} // namespace pos
