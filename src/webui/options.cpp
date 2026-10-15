// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <webui/options.h>

#include <webui/appconfig.h>

#include <common/args.h>
#include <common/settings.h>
#include <common/system.h>
#include <interfaces/node.h>
#include <kernel/caches.h>
#include <mapport.h>
#include <net.h>
#include <netbase.h>
#include <node/caches.h>
#include <node/chainstatemanager_args.h>
#include <tinyformat.h>
#include <util/chaintype.h>
#include <util/fs.h>
#include <util/strencodings.h>
#include <validation.h>
#ifdef ENABLE_WALLET
#include <wallet/wallet.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unistd.h>

#ifdef WIN32
#include <util/fs_helpers.h>

#include <shlobj.h>
#include <shobjidl.h>
#include <objbase.h>
#endif

namespace webui {
namespace {
constexpr const char* DEFAULT_GUI_PROXY_HOST{"127.0.0.1"};
constexpr uint16_t DEFAULT_GUI_PROXY_PORT{9050};
constexpr int DEFAULT_PRUNE_TARGET_GB{2};
constexpr uint64_t GB_BYTES{1'000'000'000};
#ifdef ENABLE_WALLET
constexpr bool DEFAULT_SPEND_ZEROCONF{wallet::DEFAULT_SPEND_ZEROCONF_CHANGE};
#else
constexpr bool DEFAULT_SPEND_ZEROCONF{true};
#endif

int64_t PruneGBtoMiB(int64_t gb) { return gb * GB_BYTES / (1024 * 1024); }
int PruneMiBtoGB(int64_t mib) { return (mib * 1024 * 1024 + GB_BYTES - 1) / GB_BYTES; }

//! -prune=1 is the manual pruning mode: not pruning, as far as the settings are concerned.
bool PruneEnabled(const common::SettingsValue& prune) { return SettingTo<int64_t>(prune, 0) > 1; }
int PruneSizeGB(const common::SettingsValue& prune)
{
    const int64_t value{SettingTo<int64_t>(prune, 0)};
    return value > 1 ? PruneMiBtoGB(value) : DEFAULT_PRUNE_TARGET_GB;
}
common::SettingsValue PruneSetting(bool enabled, int size_gb) { return enabled ? PruneGBtoMiB(size_gb) : 0; }

struct ProxySetting {
    bool is_set;
    std::string ip;
    std::string port;
};

ProxySetting ParseProxyString(const std::string& proxy)
{
    static const ProxySetting default_val{false, DEFAULT_GUI_PROXY_HOST, strprintf("%d", DEFAULT_GUI_PROXY_PORT)};
    if (proxy.empty()) return default_val;
    uint16_t port{0};
    std::string hostname;
    if (SplitHostPort(proxy, port, hostname) && port != 0) {
        // An IPv6 address goes in brackets.
        if (hostname.find(':') != std::string::npos) hostname = "[" + hostname + "]";
        return {true, hostname, strprintf("%d", port)};
    }
    return default_val;
}

std::string ProxyString(bool is_set, const std::string& ip, const std::string& port) { return is_set ? ip + ":" + port : ""; }

std::string DefaultProxyAddress() { return strprintf("%s:%d", DEFAULT_GUI_PROXY_HOST, DEFAULT_GUI_PROXY_PORT); }

bool Bool(const UniValue& value)
{
    if (!value.isBool()) throw std::runtime_error("true or false is needed");
    return value.get_bool();
}

int64_t Int(const UniValue& value)
{
    if (value.isNum()) return value.getInt<int64_t>();
    if (value.isStr()) {
        if (const auto n{ToIntegral<int64_t>(value.get_str())}) return *n;
    }
    throw std::runtime_error("a whole number is needed");
}

std::string Str(const UniValue& value)
{
    if (value.isNull()) return "";
    if (!value.isStr()) throw std::runtime_error("text is needed");
    return value.get_str();
}

#if !defined(WIN32) && !defined(__APPLE__)
fs::path AutostartDir()
{
    if (const char* config{std::getenv("XDG_CONFIG_HOME")}; config && *config) return fs::PathFromString(config) / "autostart";
    if (const char* home{std::getenv("HOME")}; home && *home) return fs::PathFromString(home) / ".config" / "autostart";
    return {};
}

fs::path AutostartFile(ArgsManager& args)
{
    const ChainType chain{args.GetChainType()};
    if (chain == ChainType::MAIN) return AutostartDir() / "chains-app.desktop";
    return AutostartDir() / fs::u8path(strprintf("chains-app-%s.desktop", ChainTypeToString(chain)));
}
#endif

#ifdef WIN32
//! The link in the startup folder of the user, as chains-qt (GUIUtil::StartupShortcutPath).
fs::path StartupShortcutPath(ArgsManager& args)
{
    const ChainType chain{args.GetChainType()};
    if (chain == ChainType::MAIN) return GetSpecialFolderPath(CSIDL_STARTUP) / "Chains.lnk";
    return GetSpecialFolderPath(CSIDL_STARTUP) / fs::u8path(strprintf("Chains (%s).lnk", ChainTypeToString(chain)));
}

//! COM on this thread for as long as it lives.
struct ComScope {
    bool ok{SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))};
    ~ComScope()
    {
        if (ok) CoUninitialize();
    }
};
#endif
} // namespace

NodeOptions::NodeOptions(interfaces::Node& node, ArgsManager& args, RestartFlag restart, Journal journal, ChooseProgramFn choose_program)
    : m_node(node), m_args(args), m_restart(std::move(restart)), m_journal(std::move(journal)), m_choose_program(std::move(choose_program))
{
}

bool NodeOptions::NamesProgram(const std::string& name)
{
    return name == "signer";
}

UniValue NodeOptions::ChooseProgram(const std::string& name)
{
    if (!NamesProgram(name)) throw std::runtime_error("No option naming a program: " + name);
    if (m_node.isSettingIgnored(name)) throw std::runtime_error("This option is set on the command line or in the configuration file.");
    if (!m_choose_program) throw std::runtime_error("Programs cannot be chosen here.");
    const std::string current{SettingToString(m_node.getPersistentSetting(name), "")};
    const auto chosen{m_choose_program("External signer: the HWI compatible program that talks to your hardware wallet", current)};
    if (!chosen) return UniValue{};
    if (*chosen != current) {
        if (m_journal) m_journal(name);
        m_node.updateRwSetting(name, *chosen);
        m_restart(true);
    }
    return Get();
}

bool NodeOptions::ValidProxyAddress(const std::string& text)
{
    uint16_t port{0};
    std::string hostname;
    // An address without a port: the port has a field of its own.
    if (!SplitHostPort(text, port, hostname) || port != 0) return false;
    const CService service{LookupNumeric(text, DEFAULT_GUI_PROXY_PORT)};
    return Proxy{service, /*tor_stream_isolation=*/true}.IsValid();
}

bool NodeOptions::StartAtLogin(ArgsManager& args)
{
#if !defined(WIN32) && !defined(__APPLE__)
    std::ifstream file{AutostartFile(args).std_path()};
    if (!file.good()) return false;
    std::string line;
    while (std::getline(file, line)) {
        if (line.find("Hidden") != std::string::npos && line.find("true") != std::string::npos) return false;
    }
    return true;
#elif defined(WIN32)
    std::error_code ec;
    return std::filesystem::exists(StartupShortcutPath(args), ec);
#else
    // TODO(macos port): the login items of macOS.
    (void)args;
    return false;
#endif
}

bool NodeOptions::SetStartAtLogin(ArgsManager& args, bool start)
{
#if !defined(WIN32) && !defined(__APPLE__)
    const fs::path file_path{AutostartFile(args)};
    if (!start) {
        std::error_code ec;
        std::filesystem::remove(file_path, ec);
        return !ec;
    }
    char exe[4096];
    const ssize_t r{readlink("/proc/self/exe", exe, sizeof(exe) - 1)};
    if (r <= 0) return false;
    exe[r] = '\0';
    std::error_code ec;
    std::filesystem::create_directories(AutostartDir(), ec);
    std::ofstream file{file_path.std_path(), std::ios_base::out | std::ios_base::trunc};
    if (!file.good()) return false;
    const ChainType chain{args.GetChainType()};
    file << "[Desktop Entry]\n";
    file << "Type=Application\n";
    file << (chain == ChainType::MAIN ? std::string{"Name=Chains\n"} : strprintf("Name=Chains (%s)\n", ChainTypeToString(chain)));
    file << "Exec=" << exe << strprintf(" -min -chain=%s\n", ChainTypeToString(chain));
    file << "Terminal=false\n";
    file << "Hidden=false\n";
    return file.good();
#elif defined(WIN32)
    const fs::path link_path{StartupShortcutPath(args)};
    std::error_code ec;
    fs::remove(link_path, ec);
    if (!start) return !std::filesystem::exists(link_path, ec);
    ComScope com;
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;
    IShellLinkW* link{nullptr};
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&link)))) return false;
    const std::wstring arguments{fs::u8path(strprintf("-min -chain=%s", ChainTypeToString(args.GetChainType()))).wstring()};
    link->SetPath(exe);
    link->SetArguments(arguments.c_str());
    link->SetWorkingDirectory(fs::path{exe}.parent_path().wstring().c_str());
    IPersistFile* file{nullptr};
    bool ok{SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file)))};
    if (ok) {
        ok = SUCCEEDED(file->Save(link_path.wstring().c_str(), TRUE));
        file->Release();
    }
    link->Release();
    return ok;
#else
    (void)args;
    (void)start;
    return false;
#endif
}

UniValue NodeOptions::Get()
{
    const auto setting{[&](const std::string& name) { return m_node.getPersistentSetting(name); }};
    const auto proxy{[&](const std::string& name, const char* key_use, const char* key_ip, const char* key_port, UniValue& out) {
        const ProxySetting now{ParseProxyString(SettingToString(setting(name), ""))};
        // A proxy turned off shows the address it had (name-prev), or the default one.
        const ProxySetting shown{now.is_set ? now : ParseProxyString(SettingToString(setting(name + "-prev"), DefaultProxyAddress()))};
        out.pushKV(key_use, now.is_set);
        out.pushKV(key_ip, shown.ip);
        out.pushKV(key_port, shown.port);
    }};

    UniValue out{UniValue::VOBJ};
    out.pushKV("dbcache", SettingTo<int64_t>(setting("dbcache"), node::GetDefaultDBCache() / (1024 * 1024)));
    out.pushKV("par", SettingTo<int64_t>(setting("par"), DEFAULT_SCRIPTCHECK_THREADS));
    out.pushKV("prune", PruneEnabled(setting("prune")));
    out.pushKV("pruneSize", PruneEnabled(setting("prune")) ? PruneSizeGB(setting("prune")) :
                            PruneEnabled(setting("prune-prev")) ? PruneSizeGB(setting("prune-prev")) : DEFAULT_PRUNE_TARGET_GB);
    out.pushKV("spendZeroConf", SettingToBool(setting("spendzeroconfchange"), DEFAULT_SPEND_ZEROCONF));
    out.pushKV("signer", SettingToString(setting("signer"), ""));
    out.pushKV("natpmp", SettingToBool(setting("natpmp"), DEFAULT_NATPMP));
    out.pushKV("listen", SettingToBool(setting("listen"), DEFAULT_LISTEN));
    out.pushKV("server", SettingToBool(setting("server"), false));
    proxy("proxy", "proxyUse", "proxyIp", "proxyPort", out);
    proxy("onion", "torUse", "torIp", "torPort", out);
    out.pushKV("lang", SettingToString(setting("lang"), ""));
    out.pushKV("startAtLogin", StartAtLogin(m_args));

    // What the command line or chains.conf decides (OptionsModel::addOverriddenOption).
    UniValue ignored{UniValue::VOBJ};
    std::string overridden;
    for (const char* name : {"dbcache", "par", "spendzeroconfchange", "signer", "natpmp", "listen", "server", "prune", "proxy", "onion", "lang"}) {
        if (!m_node.isSettingIgnored(name)) continue;
        ignored.pushKV(name, true);
        // A flag given without a value ("-server") shows as it was given.
        const std::string value{m_args.GetArg(std::string{"-"} + name, "")};
        overridden += value.empty() ? strprintf("-%s ", name) : strprintf("-%s=%s ", name, value);
    }
    out.pushKV("ignored", std::move(ignored));
    out.pushKV("overridden", overridden);

    UniValue limits{UniValue::VOBJ};
    limits.pushKV("dbcacheMin", static_cast<int64_t>(MIN_DBCACHE_BYTES / (1024 * 1024)));
    limits.pushKV("parMin", -GetNumCores());
    limits.pushKV("parMax", MAX_SCRIPTCHECK_THREADS);
    limits.pushKV("pruneMinGB", static_cast<int64_t>((MIN_DISK_SPACE_FOR_BLOCK_FILES / GB_BYTES) + ((MIN_DISK_SPACE_FOR_BLOCK_FILES % GB_BYTES) ? 1 : 0)));
    out.pushKV("limits", std::move(limits));

    // The networks the proxy shown is used for now (OptionsDialog::updateDefaultProxyNets).
    std::string shown_proxy{out.find_value("proxyIp").get_str()};
    if (!IsUnixSocketPath(shown_proxy)) {
        const std::optional<CNetAddr> address{LookupHost(shown_proxy, /*fAllowLookup=*/false)};
        const uint16_t port{static_cast<uint16_t>(ToIntegral<uint16_t>(out.find_value("proxyPort").get_str()).value_or(0))};
        shown_proxy = CService{address.value_or(CNetAddr{}), port}.ToStringAddrPort();
    }
    UniValue reach{UniValue::VOBJ};
    const auto uses{[&](Network net) {
        const auto p{m_node.getProxy(net)};
        return p && p->ToString() == shown_proxy;
    }};
    reach.pushKV("ipv4", uses(NET_IPV4));
    reach.pushKV("ipv6", uses(NET_IPV6));
    reach.pushKV("tor", uses(NET_ONION));
    out.pushKV("proxyReach", std::move(reach));
#ifdef ENABLE_EXTERNAL_SIGNER
    out.pushKV("externalSigner", true);
#else
    out.pushKV("externalSigner", false);
#endif
    out.pushKV("restartRequired", m_restart(std::nullopt));
    return out;
}

UniValue NodeOptions::Set(const std::string& name, const UniValue& value)
{
    const auto setting{[&](const std::string& n) { return m_node.getPersistentSetting(n); }};
    // OptionsModel UpdateRwSetting: these numbers are written as strings, for the versions that read them so.
    const auto update{[&](const std::string& n, const common::SettingsValue& v) {
        const bool as_string{v.isNum() && (n.starts_with("dbcache") || n.starts_with("par") || n.starts_with("prune"))};
        if (m_journal) m_journal(n);
        m_node.updateRwSetting(n, as_string ? common::SettingsValue{v.getValStr()} : v);
    }};
    const auto restart{[&] { m_restart(true); }};

    // --- Prune: "prune" holds the size in MiB, 0 for off; "prune-prev" keeps the size while it is off ---
    const auto prune_enabled{[&](const std::string& suffix) { return PruneEnabled(setting("prune" + suffix)); }};
    std::function<int(const std::string&)> prune_size{[&](const std::string& suffix) -> int {
        if (prune_enabled(suffix)) return PruneSizeGB(setting("prune" + suffix));
        return suffix.empty() ? prune_size("-prev") : DEFAULT_PRUNE_TARGET_GB;
    }};
    std::function<void(bool, const std::string&)> set_prune;
    std::function<void(int, const std::string&)> set_prune_size;
    set_prune = [&](bool on, const std::string& suffix) {
        if (on == prune_enabled(suffix)) return;
        if (suffix.empty() && !on) set_prune(true, "-prev");
        update("prune" + suffix, PruneSetting(on, prune_size("")));
        if (suffix.empty() && on) update("prune-prev", {});
        if (suffix.empty()) restart();
    };
    set_prune_size = [&](int gb, const std::string& suffix) {
        if (gb == prune_size(suffix)) return;
        if (suffix.empty() && !prune_enabled("")) {
            set_prune_size(gb, "-prev");
        } else {
            update("prune" + suffix, PruneSetting(true, std::max(1, gb)));
        }
        if (suffix.empty() && prune_enabled("")) restart();
    };

    // --- Proxies: "proxy" / "onion" hold ip:port, empty for off; "-prev" keeps the address while off ---
    const auto proxy_now{[&](const std::string& n, const std::string& suffix) { return ParseProxyString(SettingToString(setting(n + suffix), "")); }};
    std::function<std::string(const std::string&, bool, const std::string&)> proxy_field{[&](const std::string& n, bool ip, const std::string& suffix) -> std::string {
        const ProxySetting p{proxy_now(n, suffix)};
        if (p.is_set) return ip ? p.ip : p.port;
        if (suffix.empty()) return proxy_field(n, ip, "-prev");
        const ProxySetting d{ParseProxyString(DefaultProxyAddress())};
        return ip ? d.ip : d.port;
    }};
    std::function<void(const std::string&, bool, const std::string&)> set_proxy_use;
    std::function<void(const std::string&, bool, const std::string&, const std::string&)> set_proxy_field;
    set_proxy_use = [&](const std::string& n, bool on, const std::string& suffix) {
        if (on == proxy_now(n, suffix).is_set) return;
        if (suffix.empty() && !on) set_proxy_use(n, true, "-prev");
        update(n + suffix, ProxyString(on, proxy_field(n, true, ""), proxy_field(n, false, "")));
        if (suffix.empty() && on) update(n + "-prev", {});
        if (suffix.empty()) restart();
    };
    set_proxy_field = [&](const std::string& n, bool ip, const std::string& text, const std::string& suffix) {
        if (text == proxy_field(n, ip, suffix)) return;
        const bool used{proxy_now(n, "").is_set};
        if (suffix.empty() && !used) {
            set_proxy_field(n, ip, text, "-prev");
        } else {
            update(n + suffix, ProxyString(true, ip ? text : proxy_field(n, true, ""), ip ? proxy_field(n, false, "") : text));
        }
        if (suffix.empty() && used) restart();
    };

    const auto set_number{[&](const std::string& n, int64_t v, int64_t current) {
        if (v == current) return;
        update(n, v);
        restart();
    }};
    const auto set_flag{[&](const std::string& n, bool v, bool current) {
        if (v == current) return;
        update(n, v);
        restart();
    }};
    const auto set_text{[&](const std::string& n, const std::string& v) {
        if (v == SettingToString(setting(n), "")) return;
        update(n, v);
        restart();
    }};

    const UniValue now{Get()};
    if (name == "dbcache") {
        const int64_t mib{Int(value)};
        if (mib < now["limits"]["dbcacheMin"].getInt<int64_t>()) throw std::runtime_error(strprintf("The database cache is at least %d MiB", now["limits"]["dbcacheMin"].getInt<int64_t>()));
        set_number("dbcache", mib, now["dbcache"].getInt<int64_t>());
    } else if (name == "par") {
        const int64_t threads{Int(value)};
        if (threads < now["limits"]["parMin"].getInt<int64_t>() || threads > now["limits"]["parMax"].getInt<int64_t>()) throw std::runtime_error("This number of script verification threads is out of range");
        set_number("par", threads, now["par"].getInt<int64_t>());
    } else if (name == "prune") {
        set_prune(Bool(value), "");
    } else if (name == "pruneSize") {
        const int64_t gb{Int(value)};
        if (gb < now["limits"]["pruneMinGB"].getInt<int64_t>() || gb > std::numeric_limits<int>::max()) throw std::runtime_error("This prune size is out of range");
        set_prune_size(static_cast<int>(gb), "");
    } else if (name == "spendZeroConf") {
        set_flag("spendzeroconfchange", Bool(value), now["spendZeroConf"].get_bool());
    } else if (NamesProgram(name)) {
        // A program the node runs is chosen by the user (ChooseProgram), never a text of the page: the page
        // can only take it away.
        if (!Str(value).empty()) throw std::runtime_error("The program is chosen with Choose…, in a dialog of the desktop.");
        set_text(name, "");
    } else if (name == "natpmp") {
        // Mapping the port can change while the node runs.
        const bool on{Bool(value)};
        if (on != now["natpmp"].get_bool()) {
            update("natpmp", on);
            m_node.mapPort(on);
        }
    } else if (name == "listen") {
        set_flag("listen", Bool(value), now["listen"].get_bool());
    } else if (name == "server") {
        set_flag("server", Bool(value), now["server"].get_bool());
    } else if (name == "proxyUse" || name == "torUse") {
        set_proxy_use(name == "proxyUse" ? "proxy" : "onion", Bool(value), "");
    } else if (name == "proxyIp" || name == "torIp") {
        const std::string ip{Str(value)};
        if (!ValidProxyAddress(ip)) throw std::runtime_error("The supplied proxy address is invalid.");
        set_proxy_field(name == "proxyIp" ? "proxy" : "onion", true, ip, "");
    } else if (name == "proxyPort" || name == "torPort") {
        const int64_t port{Int(value)};
        if (port < 1 || port > 65535) throw std::runtime_error("The supplied proxy address is invalid.");
        set_proxy_field(name == "proxyPort" ? "proxy" : "onion", false, strprintf("%d", port), "");
    } else if (name == "lang") {
        set_text("lang", Str(value));
    } else if (name == "startAtLogin") {
        if (m_journal && Bool(value) != StartAtLogin(m_args)) m_journal("@startAtLogin");
        if (!SetStartAtLogin(m_args, Bool(value))) throw std::runtime_error("The app could not be set to start at login");
    } else {
        throw std::runtime_error("No option " + name);
    }
    return Get();
}

OptionsJournal::OptionsJournal(AppConfig& config, ArgsManager& args, KeyFn key) : m_config(config), m_args(args), m_key(std::move(key)) {}

namespace {
constexpr const char* RESTART_KEY{"@restartRequired"};
} // namespace

void OptionsJournal::Record(interfaces::Node& node, const std::string& setting, bool restart_required)
{
    LOCK(m_mutex);
    const std::string key{m_key()};
    UniValue undo{m_config.Value(key)};
    if (!undo.isObject()) undo = UniValue{UniValue::VOBJ};
    if (undo.exists(setting)) return;
    if (!undo.exists(RESTART_KEY)) undo.pushKV(RESTART_KEY, restart_required);
    if (setting == "@startAtLogin") {
        undo.pushKV(setting, NodeOptions::StartAtLogin(m_args));
    } else {
        UniValue entry{UniValue::VOBJ};
        entry.pushKV("value", node.getPersistentSetting(setting));
        undo.pushKV(setting, std::move(entry));
    }
    m_config.SetValue(key, undo);
}

size_t OptionsJournal::Pending() const
{
    LOCK(m_mutex);
    const UniValue undo{m_config.Value(m_key())};
    if (!undo.isObject()) return 0;
    return undo.size() - (undo.exists(RESTART_KEY) ? 1 : 0);
}

void OptionsJournal::Commit()
{
    LOCK(m_mutex);
    m_config.SetValue(m_key(), UniValue{});
}

size_t OptionsJournal::Undo(interfaces::Node& node, const std::function<void(bool)>& restart)
{
    LOCK(m_mutex);
    const std::string key{m_key()};
    const UniValue undo{m_config.Value(key)};
    if (!undo.isObject() || undo.empty()) return 0;
    std::optional<bool> natpmp;
    size_t count{0};
    for (size_t i{0}; i < undo.size(); ++i) {
        const std::string& setting{undo.getKeys()[i]};
        const UniValue& old{undo.getValues()[i]};
        if (setting == RESTART_KEY) {
            if (restart) restart(old.isBool() && old.get_bool());
            continue;
        }
        ++count;
        if (setting == "@startAtLogin") {
            if (old.isBool()) NodeOptions::SetStartAtLogin(m_args, old.get_bool());
            continue;
        }
        const UniValue& value{old.find_value("value")};
        node.updateRwSetting(setting, value);
        if (setting == "natpmp") natpmp = value.isBool() ? value.get_bool() : m_args.GetBoolArg("-natpmp", DEFAULT_NATPMP);
    }
    if (natpmp) node.mapPort(*natpmp);
    m_config.SetValue(key, UniValue{});
    return count;
}

} // namespace webui
