// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/protection.h>

#include <tinyformat.h>
#include <util/fs_helpers.h>
#include <util/strencodings.h>
#include <util/string.h>

#include <cstdio>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>

namespace pos {

namespace {
constexpr const char* HEADER{"fastblocks-slashing-protection 1"};

//! The locks this process holds: the filesystem lock does not tell one holder from another within a process.
std::mutex g_locks_mutex;
std::set<fs::path> g_locks;

fs::path LockPath(const fs::path& dir, const fs::path& name) { return fs::absolute(dir / name); }
} // namespace

std::unique_ptr<SlashingProtection> SlashingProtection::Open(const fs::path& path, const fs::path& lock_dir, const fs::path& lock_name, std::string& error)
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
    std::unique_ptr<SlashingProtection> protection{new SlashingProtection(path, lock_dir, lock_name)};
    std::ifstream file{path.std_path()};
    if (file.is_open()) {
        std::string line;
        if (!std::getline(file, line) || line != HEADER) {
            error = strprintf("%s is not a slashing protection file", fs::PathToString(path));
            return nullptr;
        }
        LOCK(protection->m_mutex);
        while (std::getline(file, line)) {
            if (line.empty()) continue;
            const auto parts{util::SplitString(line, ' ')};
            const auto key{parts.size() == 2 ? TryParseHex<uint8_t>(parts[0]) : std::nullopt};
            const auto slot{parts.size() == 2 ? ToIntegral<int64_t>(parts[1]) : std::nullopt};
            if (!key || key->size() != 32 || !slot) {
                error = strprintf("%s has a line that does not read: %s", fs::PathToString(path), line);
                return nullptr;
            }
            auto& last{protection->m_slots[uint256{std::span<const unsigned char>{*key}}]};
            last = std::max(last, *slot);
        }
    }
    return protection;
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
    text << HEADER << '\n';
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
