// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POS_BOND_H
#define BITCOIN_POS_BOND_H

#include <consensus/amount.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <uint256.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

/**
 * The script formats of staking (doc/fastblocks.md, "Staking"), apart from its rules (pos/staking.h)
 * because wallets need them too. Each is an OP_RETURN output with one push that starts with a tag.
 */
namespace pos {

//! BOND output: OP_RETURN with BOND_TAG | validator x-only key (32) | owner key hash (20) | proof of possession (64)
//! [| owner signature (65)]: the owner's signature (BondMessage) is due on every bond to a validator that has an owner.
inline constexpr unsigned char BOND_TAG[4]{0xDB, 'S', 'T', 'K'};
//! UNBOND output (value 0): OP_RETURN with UNBOND_TAG | validator key (32) | amount (8) | sequence (4) | owner signature (65).
//! An amount of UNBOND_ALL unbonds all that is bonded when the unbond is applied.
inline constexpr unsigned char UNBOND_TAG[4]{0xDC, 'U', 'N', 'B'};

//! The amount of an unbond of all that is bonded, whatever that is when it is applied.
inline constexpr CAmount UNBOND_ALL{0};

/** A bond, as read from an output. */
struct Bond {
    uint256 pubkey;
    //! The key that may unbond, and is paid what is unbonded (P2WPKH).
    uint160 owner;
    //! BIP340 signature by the validator key of BondPopHash(owner, genesis): no one bonds a key not theirs.
    std::array<unsigned char, 64> pop{};
    //! The owner's signature of BondMessage, in the format of signed messages: none on a first bond.
    std::vector<unsigned char> owner_sig;
    CAmount amount{0};
};

/** An unbond, as read from an output. */
struct Unbond {
    uint256 pubkey;
    //! UNBOND_ALL: all that is bonded then.
    CAmount amount{0};
    //! The validator's unbond_seq: an unbond counts once.
    uint32_t sequence{0};
    //! Signature of UnbondMessage by the owner key, in the format of signed messages.
    std::vector<unsigned char> signature;
};

CScript BondScript(const uint256& pubkey, const uint160& owner, const std::array<unsigned char, 64>& pop, std::span<const unsigned char> owner_sig = {});
CScript UnbondScript(const Unbond& unbond);
/** Whether a script claims to be a bond (or an unbond), well formed or not. */
bool IsBondScript(const CScript& script);
bool IsUnbondScript(const CScript& script);
/** The bond of an output; nothing if it is not one or does not read. */
std::optional<Bond> ParseBond(const CTxOut& out);
std::optional<Unbond> ParseUnbond(const CTxOut& out);
/** What the validator key signs to bond for `owner` on the chain of `genesis_hash`. */
uint256 BondPopHash(const uint160& owner, const uint256& genesis_hash);
bool CheckBondPop(const uint256& pubkey, const uint160& owner, const std::array<unsigned char, 64>& pop, const uint256& genesis_hash);
/** The message the owner signs to unbond ("all" for UNBOND_ALL). */
std::string UnbondMessage(const uint256& pubkey, CAmount amount, uint32_t sequence, const uint256& genesis_hash);
/**
 * The message the owner signs to add a bond to its validator: the amount, and the first input of the
 * transaction (a coin is spent once: the signature is good for that transaction only).
 */
std::string BondMessage(const uint256& pubkey, CAmount amount, const COutPoint& spent, const uint256& genesis_hash);

} // namespace pos

#endif // BITCOIN_POS_BOND_H
