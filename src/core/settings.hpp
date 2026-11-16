// QSettings-compatible INI storage.
//
// Keys are slash-separated paths ("Save/filename"). The file format matches Qt's IniFormat
// ([General] for top-level keys, nested groups written as "Group\key", quoted/escaped values,
// comma-separated lists) so profiles and site defaults.ini files from the original Grabber are
// read unchanged. A Settings object can layer read-only fallback files (system config,
// site defaults) under the writable one.
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/util.hpp"

namespace bg {

struct IniValue {
    StringList items;
    bool isList = false;
    bool invalid = false;  // @Invalid()
    std::string str() const;
};

class IniFile {
public:
    bool load(const std::string& path);
    bool save(const std::string& path) const;
    void parse(const std::string& data);
    std::string serialize() const;
    std::map<std::string, IniValue> values;  // full key path -> value
};

class Settings {
public:
    explicit Settings(std::string path, std::vector<std::string> fallbacks = {});
    ~Settings();
    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;

    const std::string& path() const { return m_path; }
    void reload();
    bool sync();

    bool contains(const std::string& key) const;
    bool containsOwn(const std::string& key) const;  // only the writable layer
    std::string get(const std::string& key, const std::string& def = "") const;
    long long getInt(const std::string& key, long long def = 0) const;
    bool getBool(const std::string& key, bool def = false) const;
    double getDouble(const std::string& key, double def = 0) const;
    StringList getList(const std::string& key, const StringList& def = {}) const;

    void set(const std::string& key, const std::string& value);
    void setInt(const std::string& key, long long value) { set(key, std::to_string(value)); }
    void setBool(const std::string& key, bool value) { set(key, value ? "true" : "false"); }
    void setDouble(const std::string& key, double value) { set(key, fromDouble(value)); }
    void setList(const std::string& key, const StringList& value);
    // Store value, but drop the override when it equals the fallback layer or def (MixedSettings semantics).
    void setWithDefault(const std::string& key, const std::string& value, const std::string& def);
    void remove(const std::string& key);  // removes the key and all keys below it

    StringList childKeys(const std::string& group) const;
    StringList childGroups(const std::string& group) const;
    StringList allKeys(const std::string& group) const;
    // QSettings arrays: group/size and group/<1..n>/key
    std::vector<StringMap> readArray(const std::string& group) const;
    void writeArray(const std::string& group, const std::vector<StringMap>& items);
    void renameGroup(const std::string& from, const std::string& to);

    Signal<> changed;

private:
    const IniValue* find(const std::string& key) const;
    void markDirty();

    std::string m_path;
    std::vector<std::string> m_fallbackPaths;
    IniFile m_own;
    std::vector<IniFile> m_fallbacks;
    bool m_dirty = false;
    unsigned m_syncSource = 0;
    mutable std::recursive_mutex m_mutex;
};

}  // namespace bg
