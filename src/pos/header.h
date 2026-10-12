// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POS_HEADER_H
#define BITCOIN_POS_HEADER_H

#include <consensus/amount.h>
#include <pos/evidence.h>
#include <pos/params.h>
#include <primitives/block.h>
#include <script/script.h>
#include <serialize.h>
#include <uint256.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

/**
 * FastBlocks' block formats (doc/fastblocks.md, "Blocks").
 *
 * A PoS block is an extended block of the template (BLOCK_EXTENSION_BIT): its header carries the
 * hash of its sections and the signature of the slot's leader. In the 80 bytes:
 *  - nTime is the start of its slot, exactly;
 *  - nBits is the proof-of-work limit of the chain, a constant (there is no proof of work);
 *  - nNonce is the mainchain height of the anchor the block acknowledges, 0 if none: it is in the
 *    header, and so in the block hash, because the order of chains depends on it (pos/forkchoice.h).
 * Its sections hold the acknowledgement of an anchor, if any, and proofs that leaders signed twice.
 *
 * An anchor ticket is what the mainchain commits to (BIP301, through createbmmrequest): the hash of
 * a block it anchors and of who is paid the fee pot for it.
 */
namespace pos {

//! The header bit of a PoS block: the template's extension bit.
inline constexpr int32_t POS_BLOCK_BIT{BLOCK_EXTENSION_BIT};

//! Coinbase output of a PoS block that puts coins in the fee pot: OP_RETURN with this 4-byte push.
inline constexpr unsigned char POT_TAG[4]{0xDE, 0x50, 0x4F, 0x54};

//! The largest payee script a ticket may name.
inline constexpr size_t MAX_PAYEE_SIZE{80};

/** Whether a header is that of a PoS block. */
bool IsPosHeader(const CBlockHeader& header);

/** The slot a time falls in (0 before the first block). */
int64_t SlotOf(int64_t time, const Consensus::PosParams& params);
/** The start of a slot. */
int64_t SlotTime(int64_t slot, const Consensus::PosParams& params);
/** The epoch of a slot. */
int64_t EpochOf(int64_t slot, const Consensus::PosParams& params);

/** An anchor ticket: what a bidder has the mainchain commit to. */
struct AnchorTicket {
    uint8_t version{1};
    //! The block anchored: P.
    uint256 anchored;
    //! Who the fee pot is paid to when the anchor is acknowledged.
    CScript payee;

    SERIALIZE_METHODS(AnchorTicket, obj) { READWRITE(obj.version, obj.anchored, obj.payee); }
    friend bool operator==(const AnchorTicket&, const AnchorTicket&) = default;

    /** h*: the 32 bytes the mainchain commits to. */
    uint256 Hash() const;
};

/** The acknowledgement of an anchor, in a PoS block. */
struct AnchorAck {
    AnchorTicket ticket;
    //! The height of the mainchain block that committed to the ticket (first).
    int32_t main_height{0};

    SERIALIZE_METHODS(AnchorAck, obj) { READWRITE(obj.ticket, obj.main_height); }
    friend bool operator==(const AnchorAck&, const AnchorAck&) = default;
};

//! Proofs a block of stake may carry, at most.
inline constexpr size_t MAX_SECTION_EVIDENCE{16};

/** The sections of a PoS block: an acknowledgement, and proofs that leaders signed twice. */
struct PosSections {
    std::optional<AnchorAck> ack;
    std::vector<DoubleProposal> evidence;

    /** The bytes of CBlock::vSections: none when there is nothing. */
    std::vector<unsigned char> Encode() const;
    /** Read them back; false if they are not the encoding of sections (so not empty when empty is due). */
    static bool Decode(std::span<const unsigned char> bytes, PosSections& sections);
    friend bool operator==(const PosSections&, const PosSections&) = default;
};

/** The hash of a block's sections, as its header commits to it. */
uint256 SectionsHash(std::span<const unsigned char> sections);
/** What the leader of the slot signs: the block hash, tagged. */
uint256 BlockSigHash(const CBlockHeader& header);
/** The leader's signature is valid for this header. */
bool CheckBlockSig(const CBlockHeader& header, const uint256& xonly_pubkey);

/** The script of a coinbase output that puts coins in the fee pot. */
CScript PotScript();
bool IsPotScript(const CScript& script);
/** Whether a ticket's payee is one: no larger than MAX_PAYEE_SIZE, standard, and not data. */
bool IsValidPayee(const CScript& payee);

/** The nBits of every PoS block: the chain's proof-of-work limit. */
uint32_t PosBits(const uint256& pow_limit);

} // namespace pos

#endif // BITCOIN_POS_HEADER_H
