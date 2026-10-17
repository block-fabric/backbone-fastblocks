// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/protection.h>

#include <tinyformat.h>
#include <util/fs_helpers.h>
#include <util/strencodings.h>
#include <util/string.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>

namespace pos {

namespace {
//! The first format, and the second, which names the chain's genesis after it.
constexpr const char* HEADER{"fastblocks-slashing-protection 1"};
constexpr const char* HEADER_V2{"fastblocks-slashing-protection 2"};

//! The locks this process holds: the filesystem lock does not tell one holder from another within a process.
std::mutex g_locks_mutex;
std::set<fs::path> g_locks;

fs::path LockPath(const fs::path& dir, const fs::path& name) { return fs::absolute(dir / name); }
} // namespace

std::unique_ptr<SlashingProtection> SlashingProtection::Open(const fs::path& path, const fs::path& lock_dir, const fs::path& lock_name, std::string& error,
                                                             const std::vector<fs::path>& merge_from, const std::optional<uint256>& genesis)
{
    {
        std::lock_guard lock{g_locks_mutex};
        if (g_locks.contains(LockPath(lock_dir, lock_name))) {
            error = strprintf("another validator of this process uses the keys of %s", fs::PathToString(lock_dir / lock_name));
            return nullptr;
        }
        if (util::LockDirectory(lock_dir, lock_name) != util::LockResult::Success) {
            error = strprintf("cannot lock %s: is another node running with these validator keys?", fs::PathToString(lock_dir / lock_name));
            return nullptr;
        }
        g_locks.insert(LockPath(lock_dir, lock_name));
    }
    // Made once the lock is taken (its destructor gives it back), and loaded in its constructor (m_slots
    // is set before anyone else can see the object).
    bool loaded{false};
    std::unique_ptr<SlashingProtection> protection{new SlashingProtection(path, lock_dir, lock_name, merge_from, genesis, error, loaded)};
    if (!loaded) return nullptr;
    return protection;
}

SlashingProtection::SlashingProtection(fs::path path, fs::path lock_dir, fs::path lock_name, const std::vector<fs::path>& merge_from, std::optional<uint256> genesis, std::string& error, bool& loaded)
    : m_path{std::move(path)}, m_lock_dir{std::move(lock_dir)}, m_lock_name{std::move(lock_name)}, m_genesis{std::move(genesis)}
{
    std::map<uint256, int64_t> slots;
    std::optional<uint256> other_genesis;
    if (!ReadSlots(m_path, slots, error, m_genesis, other_genesis)) return;
    // A file of another chain (a test network restarted from a new genesis): kept beside, and this
    // file starts again for this chain.
    bool rewrite{false};
    if (other_genesis) {
        const fs::path old{fs::PathFromString(fs::PathToString(m_path) + "." + other_genesis->GetHex() + ".old")};
        if (!RenameOver(m_path, old)) {
            error = strprintf("cannot move %s aside (it is of another chain, genesis %s)", fs::PathToString(m_path), other_genesis->GetHex());
            return;
        }
        rewrite = true;
    }
    // Files kept elsewhere before (an older version of the node): what they hold counts too, the highest
    // slot of each key, and goes into this file at once. A file that does not read stops the node: it
    // may hold the last slot a key signed.
    bool merged{false};
    for (const fs::path& other : merge_from) {
        std::map<uint256, int64_t> other_slots;
        std::optional<uint256> ignored;
        if (!ReadSlots(other, other_slots, error, m_genesis, ignored)) return;
        for (const auto& [key, slot] : other_slots) {
            auto [it, inserted]{slots.try_emplace(key, slot)};
            if (inserted || it->second < slot) {
                it->second = slot;
                merged = true;
            }
        }
    }
    if ((merged || rewrite) && !Write(slots, error)) return;
    m_slots = std::move(slots);
    loaded = true;
}

bool SlashingProtection::ReadSlots(const fs::path& path, std::map<uint256, int64_t>& slots, std::string& error, const std::optional<uint256>& genesis,
                                   std::optional<uint256>& other_genesis)
{
    std::ifstream file{path.std_path()};
    if (!file.is_open()) return true;
    std::string line;
    if (!std::getline(file, line)) line.clear();
    std::optional<uint256> named;
    if (line.starts_with(std::string{HEADER_V2} + " ")) {
        named = uint256::FromHex(line.substr(std::string{HEADER_V2}.size() + 1));
        if (!named) {
            error = strprintf("%s names no genesis that reads: %s", fs::PathToString(path), line);
            return false;
        }
    } else if (line != HEADER) {
        error = strprintf("%s is not a slashing protection file", fs::PathToString(path));
        return false;
    }
    std::map<uint256, int64_t> read;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        const auto parts{util::SplitString(line, ' ')};
        const auto key{parts.size() == 2 ? TryParseHex<uint8_t>(parts[0]) : std::nullopt};
        const auto slot{parts.size() == 2 ? ToIntegral<int64_t>(parts[1]) : std::nullopt};
        if (!key || key->size() != 32 || !slot) {
            error = strprintf("%s has a line that does not read: %s", fs::PathToString(path), line);
            return false;
        }
        const uint256 pubkey{std::span<const unsigned char>{*key}};
        auto [it, inserted]{read.try_emplace(pubkey, *slot)};
        if (!inserted) it->second = std::max(it->second, *slot);
    }
    // (Read in full first: a file that does not read stops the node, whatever chain it names.) A file
    // of another chain: none of its slots count.
    if (named && genesis && *named != *genesis) {
        other_genesis = named;
        return true;
    }
    for (const auto& [pubkey, slot] : read) {
        auto [it, inserted]{slots.try_emplace(pubkey, slot)};
        if (!inserted) it->second = std::max(it->second, slot);
    }
    return true;
}

SlashingProtection::~SlashingProtection()
{
    std::lock_guard lock{g_locks_mutex};
    UnlockDirectory(m_lock_dir, m_lock_name);
    g_locks.erase(LockPath(m_lock_dir, m_lock_name));
}

std::optional<int64_t> SlashingProtection::LastSigned(const uint256& key) const
{
    LOCK(m_mutex);
    const auto found{m_slots.find(key)};
    if (found == m_slots.end()) return std::nullopt;
    return found->second;
}

bool SlashingProtection::Write(const std::map<uint256, int64_t>& slots, std::string& error) const
{
    std::ostringstream text;
    if (m_genesis) {
        text << HEADER_V2 << ' ' << m_genesis->GetHex() << '\n';
    } else {
        text << HEADER << '\n';
    }
    for (const auto& [key, slot] : slots) text << HexStr(key) << ' ' << slot << '\n';
    const std::string bytes{text.str()};
    const fs::path temp{fs::PathFromString(fs::PathToString(m_path) + ".new")};
    FILE* file{fsbridge::fopen(temp, "wb")};
    if (!file) {
        error = strprintf("cannot write %s", fs::PathToString(temp));
        return false;
    }
    const bool written{std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size() && FileCommit(file)};
    if (std::fclose(file) != 0 || !written) {
        error = strprintf("cannot write %s", fs::PathToString(temp));
        return false;
    }
    if (!RenameOver(temp, m_path)) {
        error = strprintf("cannot replace %s", fs::PathToString(m_path));
        return false;
    }
    DirectoryCommit(m_path.parent_path());
    return true;
}

bool SlashingProtection::MaySign(const uint256& key, int64_t slot, std::string& error)
{
    LOCK(m_mutex);
    if (const auto found{m_slots.find(key)}; found != m_slots.end() && slot <= found->second) {
        error = strprintf("slot %d is not after slot %d, the last this key signed for", slot, found->second);
        return false;
    }
    std::map<uint256, int64_t> slots{m_slots};
    slots[key] = slot;
    if (!Write(slots, error)) return false;
    m_slots = std::move(slots);
    return true;
}

} // namespace pos
