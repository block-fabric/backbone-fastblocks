// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WEBUI_OPTIONS_H
#define BITCOIN_WEBUI_OPTIONS_H

#include <sync.h>
#include <univalue.h>

#include <functional>
#include <optional>
#include <string>

class ArgsManager;
namespace interfaces {
class Node;
} // namespace interfaces

namespace webui {

/**
 * The options of the node that the Settings page changes, as OptionsModel of chains-qt
 * keeps them: in settings.json of the data directory (node.updateRwSetting), with the
 * same names and the same rules (a value set on the command line or in chains.conf wins
 * and cannot be changed here; the address of a proxy that is turned off is kept as
 * "proxy-prev"; the prune size as "prune-prev"; numbers written as strings for old
 * versions). Changes that only take effect after a restart say so.
 */
class NodeOptions
{
public:
    //! Remembers that a restart is needed (OptionsModel fRestartRequired), and tells whether it is.
    using RestartFlag = std::function<bool(std::optional<bool> set)>;

    /**
     * Told the name of a setting of settings.json just before it changes ("@startAtLogin" for the
     * start at login), so that the change can be taken back (the Cancel of the Settings page).
     */
    using Journal = std::function<void(const std::string& setting)>;

    /**
     * Have the user choose a program for a setting that names one (title, the program set now): the file
     * chooser of the desktop and a confirmation of the desktop, never a text of the page. Nothing when cancelled.
     */
    using ChooseProgramFn = std::function<std::optional<std::string>(const std::string& title, const std::string& current)>;

    NodeOptions(interfaces::Node& node, ArgsManager& args, RestartFlag restart, Journal journal = {}, ChooseProgramFn choose_program = {});

    /**
     * The settings that name a program the node runs (-signer). The page can only clear them (Set with an
     * empty value); a program is set with ChooseProgram. The others (-blocknotify, -walletnotify,
     * -alertnotify, -startupnotify, -shutdownnotify) are not settings of this page at all.
     */
    static bool NamesProgram(const std::string& name);

    /** Set a setting that names a program (NamesProgram) to one the user chooses (ChooseProgramFn). @return Get(), or null when cancelled */
    UniValue ChooseProgram(const std::string& name);

    /**
     * Every option with its value, as the Settings page shows them:
     *   dbcache, par, prune, pruneSize, spendZeroConf, signer, natpmp, listen, server,
     *   proxyUse, proxyIp, proxyPort, torUse, torIp, torPort, lang, startAtLogin;
     * ignored {setting: true} for the ones set on the command line or in chains.conf,
     * overridden (the text of chains-qt: "-par=4 -listen=0 "), limits, restartRequired,
     * proxyReach {ipv4, ipv6, tor} (the networks the proxy shown is used for now).
     */
    UniValue Get();

    /** Change one option (a name of Get). Throws std::runtime_error for a value that is not valid. @return Get() */
    UniValue Set(const std::string& name, const UniValue& value);

    /** Whether text is the address of a proxy, without a port (ProxyAddressValidator). */
    static bool ValidProxyAddress(const std::string& text);

    /** Start the app when the user logs in (GUIUtil::SetStartOnSystemStartup), per network. */
    static bool StartAtLogin(ArgsManager& args);
    static bool SetStartAtLogin(ArgsManager& args, bool start);

private:
    interfaces::Node& m_node;
    ArgsManager& m_args;
    RestartFlag m_restart;
    Journal m_journal;
    ChooseProgramFn m_choose_program;
};

class AppConfig;

/**
 * The changes of the Settings page wait for Apply (the OK of the options dialog of chains-qt). Each is
 * written to settings.json at once (NodeOptions), and what it replaced is kept here, in the settings of
 * the app (so that neither a crash nor a quit loses it): Commit forgets it, Undo puts it back. The app
 * undoes at start what was never applied, before the node reads its settings: a stray click never takes
 * effect.
 */
class OptionsJournal
{
public:
    //! The key of the journal in the settings of the app (one per network).
    using KeyFn = std::function<std::string()>;

    OptionsJournal(AppConfig& config, ArgsManager& args, KeyFn key);

    /** A setting (or "@startAtLogin") is about to change: keep what it is, the first time. */
    void Record(interfaces::Node& node, const std::string& setting, bool restart_required) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** The number of settings changed and not applied. */
    size_t Pending() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Apply: the changes stay. */
    void Commit() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /**
     * Cancel: every setting back as it was (port mapping too, the one option that took effect at once),
     * and the restart flag as it was, through `restart`. @return the number of settings put back
     */
    size_t Undo(interfaces::Node& node, const std::function<void(bool)>& restart) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

private:
    AppConfig& m_config;
    ArgsManager& m_args;
    const KeyFn m_key;
    mutable Mutex m_mutex;
};

} // namespace webui

#endif // BITCOIN_WEBUI_OPTIONS_H
