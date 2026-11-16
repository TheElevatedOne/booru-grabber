#include "core/paths.hpp"

#include "config.hpp"

namespace bg::paths {

namespace {
struct State {
    bool initialized = false;
    std::string exe, appDir;
    bool portable = false;
    std::string profile;  // single directory for everything (portable or --profile)
    StringList dataDirs;
};
State& st() {
    static State s;
    return s;
}

std::string xdgDir(const char* (*fn)(), const char* stateFallback) {
    if(fn) return buildPath(fn(), "booru-grabber");
    // $XDG_STATE_HOME (GLib >= 2.72 has g_get_user_state_dir)
    const char* env = g_getenv("XDG_STATE_HOME");
    std::string base = env && *env ? env : buildPath(g_get_home_dir(), stateFallback);
    return buildPath(base, "booru-grabber");
}
}  // namespace

void init(const std::string& profileOverride, bool forcePortable) {
    State& s = st();
    gchar* exe = g_file_read_link("/proc/self/exe", nullptr);
    s.exe = exe ? exe : "";
    g_free(exe);
    s.appDir = s.exe.empty() ? g_get_current_dir() : dirName(s.exe);

    const char* envPortable = g_getenv("BOORU_GRABBER_PORTABLE");
    s.portable = forcePortable || fileExists(buildPath(s.appDir, "portable")) || (envPortable && toBool(envPortable));
    const char* envProfile = g_getenv("BOORU_GRABBER_PROFILE");
    if(!profileOverride.empty())
        s.profile = absolutePath(profileOverride);
    else if(envProfile && *envProfile)
        s.profile = absolutePath(envProfile);
    else if(s.portable)
        s.profile = buildPath(s.appDir, "profile");

    // Shared data directories, most specific first
    StringList candidates;
    const char* envData = g_getenv("BOORU_GRABBER_DATA_DIR");
    if(envData && *envData) candidates.push_back(envData);
    candidates.push_back(buildPath(s.appDir, "share/booru-grabber"));
    candidates.push_back(buildPath(s.appDir, "../share/booru-grabber"));
    candidates.push_back(BG_INSTALL_DATADIR);
    for(const gchar* const* d = g_get_system_data_dirs(); d && *d; d++) candidates.push_back(buildPath(*d, "booru-grabber"));
    s.dataDirs.clear();
    for(auto& c : candidates) {
        std::string p = cleanPath(absolutePath(c));
        if(isDir(p) && !contains(s.dataDirs, p)) s.dataDirs.push_back(p);
    }
    s.initialized = true;
}

static State& ready() {
    if(!st().initialized) init();
    return st();
}

const std::string& executable() { return ready().exe; }
const std::string& appDir() { return ready().appDir; }
bool portable() { return ready().portable; }
bool singleProfileDir() { return !ready().profile.empty(); }
const StringList& dataDirs() { return ready().dataDirs; }

std::string findData(const std::string& rel) {
    for(auto& d : dataDirs()) {
        std::string p = buildPath(d, rel);
        if(fileExists(p)) return p;
    }
    return "";
}

std::string sitesDir() {
    std::string s = findData("sites");
    return s.empty() ? buildPath(BG_INSTALL_DATADIR, "sites") : s;
}

std::string configDir() {
    auto& s = ready();
    return s.profile.empty() ? buildPath(g_get_user_config_dir(), "booru-grabber") : s.profile;
}

std::string dataDir() {
    auto& s = ready();
    return s.profile.empty() ? buildPath(g_get_user_data_dir(), "booru-grabber") : s.profile;
}

std::string cacheDir() {
    auto& s = ready();
    return s.profile.empty() ? buildPath(g_get_user_cache_dir(), "booru-grabber") : buildPath(s.profile, "cache");
}

std::string stateDir() {
    auto& s = ready();
    if(!s.profile.empty()) return s.profile;
    return xdgDir(nullptr, ".local/state");
}

std::string settingsFile() { return buildPath(configDir(), "settings.ini"); }

StringList systemSettingsFiles() {
    auto& s = ready();
    StringList out;
    auto add = [&](const std::string& p) {
        std::string c = cleanPath(absolutePath(p));
        if(fileExists(c) && !contains(out, c)) out.push_back(c);
    };
    if(s.portable) add(buildPath(s.appDir, "etc/booru-grabber/settings.ini"));
    add(buildPath(BG_INSTALL_SYSCONFDIR, "settings.ini"));
    add(buildPath(s.appDir, "../etc/booru-grabber/settings.ini"));
    for(const gchar* const* d = g_get_system_config_dirs(); d && *d; d++) add(buildPath(*d, "booru-grabber/settings.ini"));
    return out;
}

std::string userSitesDir() { return buildPath(dataDir(), "sites"); }

}  // namespace bg::paths
