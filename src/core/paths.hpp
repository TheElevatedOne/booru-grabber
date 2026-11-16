// Locates the shared data (site sources, word list, icons) and the user profile.
//
// Shared data is looked up relative to the executable first, so one build works as
//   - an FHS install        /usr/bin + /usr/lib + /usr/share/booru-grabber + /etc/booru-grabber
//   - a prefix directory    <dir>/bin, <dir>/lib, <dir>/share (cmake --install --prefix <dir>)
//   - a portable directory  <dir>/booru-grabber, <dir>/lib, <dir>/share, <dir>/portable
//   - an AppImage           usr/bin, usr/lib, usr/share inside the image
//
// The profile follows the XDG base directories:
//   config  $XDG_CONFIG_HOME/booru-grabber   settings.ini, per-site settings
//   data    $XDG_DATA_HOME/booru-grabber     favorites, monitors, md5 database, cookies, tag databases
//   cache   $XDG_CACHE_HOME/booru-grabber    HTTP and thumbnail cache
//   state   $XDG_STATE_HOME/booru-grabber    logs
// with read-only defaults from /etc/booru-grabber/settings.ini and $XDG_CONFIG_DIRS.
// Portable mode (a "portable" file next to the binary, BOORU_GRABBER_PORTABLE=1 or --portable)
// and --profile/BOORU_GRABBER_PROFILE keep everything in one directory instead.
#pragma once

#include <string>

#include "core/util.hpp"

namespace bg::paths {

void init(const std::string& profileOverride = "", bool forcePortable = false);
const std::string& executable();
const std::string& appDir();
bool portable();
bool singleProfileDir();

const StringList& dataDirs();
std::string findData(const std::string& relative);  // first match in dataDirs(), or ""
std::string sitesDir();                              // shared site sources

std::string configDir();
std::string dataDir();
std::string cacheDir();
std::string stateDir();
std::string settingsFile();
StringList systemSettingsFiles();
std::string userSitesDir();  // user-installed sources and per-site data

}  // namespace bg::paths
