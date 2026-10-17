// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POS_PROTECTION_H
#define BITCOIN_POS_PROTECTION_H

#include <sync.h>
#include <uint256.h>
#include <util/fs.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pos {

/**
 * Slashing protection of a validator node (inspired by Ethereum's EIP-3076): the highest slot each
 * validator key signed a block for, kept in a file that is on disk before any signature leaves the
 * node. A key never signs at or below a slot it signed before, so a restarted node (systemd) never
 * signs two blocks for one slot.
 *
 * The file has a line per key: the x-only key in hex and the slot. It lives next to the validator key
 * file (so that wiping the node's data directory to sync again does not lose it), as does a lock file
 * that keeps a second node with the same keys from starting.
 */
class SlashingProtection
{
public:
    /**
     * Open (or create) the file `path`, and take the lock `lock_name` in `lock_dir`. The slots of the
     * files `merge_from` (where an older version kept them), if they exist, count too: the highest of
     * each key is written to `path` at once.
     * With `genesis` (the chain's genesis block hash), the file names it, and the slots of a file
     * written for another genesis do not count: a block signed on another chain is no block of this one
     * (the signature covers the genesis hash: BlockSigHash), so no proof that the key signed twice here
     * (audit 7, finding 3: a testnet restarted with a new genesis, and slots numbered from a later
     * genesis time, would otherwise refuse every slot until the old numbers were passed). Such a file
     * is kept beside, as `<path>.<genesis>.old`. A file of the first format (no genesis) counts.
     * @return nullptr, with `error` set, if a file does not read or the lock is held
     */
    static std::unique_ptr<SlashingProtection> Open(const fs::path& path, const fs::path& lock_dir, const fs::path& lock_name, std::string& error,
                                                    const std::vector<fs::path>& merge_from = {}, const std::optional<uint256>& genesis = std::nullopt);
    ~SlashingProtection();
    SlashingProtection(const SlashingProtection&) = delete;
    SlashingProtection& operator=(const SlashingProtection&) = delete;

    /** The highest slot `key` signed for, if any. */
    std::optional<int64_t> LastSigned(const uint256& key) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /**
     * Whether `key` may sign for `slot`: only above every slot it signed for. If so, the slot is
     * written to disk (and synced) before this returns.
     */
    bool MaySign(const uint256& key, int64_t slot, std::string& error) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

private:
    //! (With the lock taken: the destructor gives it back.) `loaded` false, with `error` set, if a file does not read.
    SlashingProtection(fs::path path, fs::path lock_dir, fs::path lock_name, const std::vector<fs::path>& merge_from, std::optional<uint256> genesis, std::string& error, bool& loaded);
    bool Write(const std::map<uint256, int64_t>& slots, std::string& error) const;
    /**
     * Add the slots of the file `path` to `slots` (the highest of each key); nothing if it does not
     * exist, or if it names a genesis other than `genesis` (then `other_genesis` is set to it).
     */
    static bool ReadSlots(const fs::path& path, std::map<uint256, int64_t>& slots, std::string& error, const std::optional<uint256>& genesis, std::optional<uint256>& other_genesis);

    const fs::path m_path;
    const fs::path m_lock_dir;
    const fs::path m_lock_name;
    const std::optional<uint256> m_genesis;
    mutable Mutex m_mutex;
    std::map<uint256, int64_t> m_slots GUARDED_BY(m_mutex);
};

} // namespace pos

#endif // BITCOIN_POS_PROTECTION_H
