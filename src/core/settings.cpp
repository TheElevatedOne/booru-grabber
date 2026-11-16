#include "core/settings.hpp"

#include <algorithm>
#include <set>

namespace bg {

std::string IniValue::str() const {
    if(invalid || items.empty()) return "";
    if(!isList || items.size() == 1) return items[0];
    return join(items, ", ");
}

// ---- parsing ---------------------------------------------------------------

static int hexVal(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void appendCodepoint(std::string& out, unsigned cp) {
    char buf[8];
    int n = g_unichar_to_utf8((gunichar)cp, buf);
    out.append(buf, (size_t)n);
}

// Keys: "%XX" / "%UXXXX" escapes, '\' separates groups.
static std::string unescapeKey(const std::string& k) {
    std::string out;
    for(size_t i = 0; i < k.size(); i++) {
        char c = k[i];
        if(c == '\\') {
            out += '/';
        } else if(c == '%' && i + 2 < k.size() + 0 && k[i + 1] == 'U' && i + 5 < k.size()) {
            unsigned cp = 0;
            bool ok = true;
            for(int j = 0; j < 4; j++) {
                int v = hexVal(k[i + 2 + j]);
                if(v < 0) ok = false;
                cp = cp * 16 + (unsigned)std::max(v, 0);
            }
            if(ok) {
                appendCodepoint(out, cp);
                i += 5;
            } else
                out += c;
        } else if(c == '%' && i + 2 < k.size() && hexVal(k[i + 1]) >= 0 && hexVal(k[i + 2]) >= 0) {
            out += (char)(hexVal(k[i + 1]) * 16 + hexVal(k[i + 2]));
            i += 2;
        } else {
            out += c;
        }
    }
    return out;
}

static std::string escapeKey(const std::string& k) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for(unsigned char c : k) {
        if(c == '/')
            out += '\\';
        else if(g_ascii_isalnum(c) || c == '_' || c == '-' || c == '.' || c == '~' || c >= 0x80)
            out += (char)c;
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

// Qt's iniUnescapedStringList: quotes, C escapes, ',' separates list items, ';' starts a comment.
static IniValue parseValue(const std::string& raw) {
    IniValue v;
    std::string s = trim(raw);
    if(startsWith(s, "@Invalid()")) {
        v.invalid = true;
        return v;
    }
    std::string cur;
    bool inQuotes = false, hadQuotes = false, isList = false;
    size_t i = 0;
    auto flush = [&](bool keepRaw) {
        std::string item = keepRaw ? cur : trim(cur);
        v.items.push_back(item);
        cur.clear();
        hadQuotes = false;
    };
    while(i < s.size()) {
        char c = s[i];
        if(c == '"') {
            inQuotes = !inQuotes;
            hadQuotes = true;
            i++;
            continue;
        }
        if(!inQuotes && c == ';') break;
        if(!inQuotes && c == ',') {
            isList = true;
            flush(hadQuotes);
            i++;
            while(i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
            continue;
        }
        if(c == '\\' && i + 1 < s.size()) {
            char e = s[++i];
            switch(e) {
                case 'a': cur += '\a'; i++; break;
                case 'b': cur += '\b'; i++; break;
                case 'f': cur += '\f'; i++; break;
                case 'n': cur += '\n'; i++; break;
                case 'r': cur += '\r'; i++; break;
                case 't': cur += '\t'; i++; break;
                case 'v': cur += '\v'; i++; break;
                case '"': case '\'': case '\\': case '?': cur += e; i++; break;
                case 'x': {
                    i++;
                    unsigned cp = 0;
                    int n = 0;
                    while(i < s.size() && hexVal(s[i]) >= 0 && n < 4) {
                        cp = cp * 16 + (unsigned)hexVal(s[i]);
                        i++;
                        n++;
                    }
                    appendCodepoint(cur, cp);
                    break;
                }
                default:
                    if(e >= '0' && e <= '7') {
                        unsigned cp = 0;
                        int n = 0;
                        while(i < s.size() && s[i] >= '0' && s[i] <= '7' && n < 3) {
                            cp = cp * 8 + (unsigned)(s[i] - '0');
                            i++;
                            n++;
                        }
                        appendCodepoint(cur, cp);
                    } else {
                        cur += e;
                        i++;
                    }
            }
            continue;
        }
        cur += c;
        i++;
    }
    // Unquoted text keeps internal spaces but loses surrounding ones; quoted keeps all.
    if(!isList) {
        v.items.push_back(hadQuotes ? cur : trim(cur));
    } else {
        flush(hadQuotes);
        v.isList = true;
    }
    // "@@x" is an escaped literal "@x"
    for(auto& it : v.items)
        if(startsWith(it, "@@")) it = it.substr(1);
    return v;
}

static std::string escapeString(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    bool needsQuotes = false, escapeNextIfDigit = false;
    std::string src = startsWith(s, "@") ? "@" + s : s;
    for(unsigned char ch : src) {
        if(ch == ';' || ch == ',' || ch == '=') needsQuotes = true;
        if(escapeNextIfDigit && g_ascii_isxdigit(ch)) {
            out += "\\x";
            out += hex[ch >> 4];
            out += hex[ch & 15];
            continue;
        }
        escapeNextIfDigit = false;
        switch(ch) {
            case '\0': out += "\\0"; escapeNextIfDigit = true; break;
            case '\a': out += "\\a"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\v': out += "\\v"; break;
            case '"':
            case '\\':
                out += '\\';
                out += (char)ch;
                break;
            default:
                if(ch <= 0x1F || ch == 0x7F) {
                    out += "\\x";
                    out += hex[ch >> 4];
                    out += hex[ch & 15];
                    escapeNextIfDigit = true;
                } else {
                    out += (char)ch;
                }
        }
    }
    if(needsQuotes || (!out.empty() && (g_ascii_isspace(out.front()) || g_ascii_isspace(out.back())))) out = "\"" + out + "\"";
    return out;
}

static std::string serializeValue(const IniValue& v) {
    if(v.invalid) return "@Invalid()";
    if(!v.isList) return v.items.empty() ? "" : escapeString(v.items[0]);
    if(v.items.empty()) return "@Invalid()";
    StringList parts;
    for(auto& it : v.items) parts.push_back(escapeString(it));
    return join(parts, ", ");
}

void IniFile::parse(const std::string& data) {
    values.clear();
    std::string section;
    // Join continuation lines is not needed for QSettings output; handle CRLF.
    for(auto rawLine : split(data, '\n')) {
        if(!rawLine.empty() && rawLine.back() == '\r') rawLine.pop_back();
        std::string line = trim(rawLine);
        if(line.empty() || line[0] == ';' || line[0] == '#') continue;
        if(line[0] == '[') {
            size_t end = line.rfind(']');
            std::string name = line.substr(1, end == std::string::npos ? std::string::npos : end - 1);
            section = unescapeKey(name);
            if(section == "General") section.clear();
            else if(section == "%General") section = "General";
            continue;
        }
        size_t eq = line.find('=');
        if(eq == std::string::npos) continue;
        std::string key = unescapeKey(trim(line.substr(0, eq)));
        if(key.empty()) continue;
        std::string full = section.empty() ? key : section + "/" + key;
        values[full] = parseValue(line.substr(eq + 1));
    }
}

bool IniFile::load(const std::string& path) {
    auto d = readFile(path);
    if(!d) {
        values.clear();
        return false;
    }
    std::string data = *d;
    if(startsWith(data, "\xEF\xBB\xBF")) data = data.substr(3);
    parse(data);
    return true;
}

std::string IniFile::serialize() const {
    std::map<std::string, std::vector<std::pair<std::string, const IniValue*>>> sections;
    for(auto& [key, value] : values) {
        size_t slash = key.find('/');
        std::string section = slash == std::string::npos ? "General" : key.substr(0, slash);
        std::string rest = slash == std::string::npos ? key : key.substr(slash + 1);
        if(slash != std::string::npos && section == "General") section = "%General";
        sections[section].emplace_back(rest, &value);
    }
    std::string out;
    auto writeSection = [&](const std::string& name) {
        auto it = sections.find(name);
        if(it == sections.end()) return;
        if(!out.empty()) out += "\n";
        out += "[" + (name == "General" || name == "%General" ? name : escapeKey(name)) + "]\n";
        for(auto& [k, v] : it->second) out += escapeKey(k) + "=" + serializeValue(*v) + "\n";
    };
    writeSection("General");
    for(auto& [name, _] : sections)
        if(name != "General") writeSection(name);
    return out;
}

bool IniFile::save(const std::string& path) const { return writeFile(path, serialize()); }

// ---- Settings --------------------------------------------------------------

Settings::Settings(std::string path, std::vector<std::string> fallbacks)
    : m_path(std::move(path)), m_fallbackPaths(std::move(fallbacks)) {
    reload();
}

Settings::~Settings() {
    if(m_syncSource) g_source_remove(m_syncSource);
    sync();
}

void Settings::reload() {
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    if(!m_path.empty()) m_own.load(m_path);
    m_fallbacks.clear();
    for(auto& p : m_fallbackPaths) {
        IniFile f;
        if(f.load(p)) m_fallbacks.push_back(std::move(f));
    }
    m_dirty = false;
}

bool Settings::sync() {
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    if(!m_dirty || m_path.empty()) return true;
    m_dirty = false;
    return m_own.save(m_path);
}

static gboolean syncLater(gpointer p) {
    auto* s = static_cast<Settings*>(p);
    s->sync();
    return G_SOURCE_REMOVE;
}

void Settings::markDirty() {
    m_dirty = true;
    if(!m_syncSource && g_main_context_default())
        m_syncSource = g_timeout_add_full(G_PRIORITY_LOW, 1000, syncLater, this, [](gpointer p) { static_cast<Settings*>(p)->m_syncSource = 0; });
    changed.emit();
}

const IniValue* Settings::find(const std::string& key) const {
    auto it = m_own.values.find(key);
    if(it != m_own.values.end()) return &it->second;
    for(auto& f : m_fallbacks) {
        auto jt = f.values.find(key);
        if(jt != f.values.end()) return &jt->second;
    }
    return nullptr;
}

bool Settings::contains(const std::string& key) const {
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    return find(key) != nullptr;
}

bool Settings::containsOwn(const std::string& key) const {
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    return m_own.values.count(key) > 0;
}

std::string Settings::get(const std::string& key, const std::string& def) const {
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    const IniValue* v = find(key);
    if(!v || v->invalid) return def;
    return v->str();
}

long long Settings::getInt(const std::string& key, long long def) const {
    std::string s = get(key);
    bool ok = false;
    long long v = toInt(s, def, &ok);
    if(!ok) {
        double d = toDouble(s, (double)def, &ok);
        return ok ? (long long)d : def;
    }
    return v;
}

bool Settings::getBool(const std::string& key, bool def) const { return toBool(get(key), def); }
double Settings::getDouble(const std::string& key, double def) const { return toDouble(get(key), def); }

StringList Settings::getList(const std::string& key, const StringList& def) const {
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    const IniValue* v = find(key);
    if(!v) return def;
    if(v->invalid) return {};
    if(v->isList) return v->items;
    if(v->items.empty() || v->items[0].empty()) return {};
    return v->items;
}

void Settings::set(const std::string& key, const std::string& value) {
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    IniValue v;
    v.items.push_back(value);
    auto it = m_own.values.find(key);
    if(it != m_own.values.end() && !it->second.isList && !it->second.invalid && it->second.items == v.items) return;
    m_own.values[key] = v;
    markDirty();
}

void Settings::setList(const std::string& key, const StringList& value) {
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    IniValue v;
    v.items = value;
    v.isList = value.size() != 1;
    m_own.values[key] = v;
    markDirty();
}

void Settings::setWithDefault(const std::string& key, const std::string& value, const std::string& def) {
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    for(auto& f : m_fallbacks) {
        auto it = f.values.find(key);
        if(it != f.values.end()) {
            if(it->second.str() == value) {
                if(m_own.values.erase(key)) markDirty();
            } else {
                set(key, value);
            }
            return;
        }
    }
    if(value == def) {
        if(m_own.values.erase(key)) markDirty();
        return;
    }
    set(key, value);
}

void Settings::remove(const std::string& key) {
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    bool changedAny = false;
    std::string prefix = key + "/";
    for(auto it = m_own.values.begin(); it != m_own.values.end();) {
        if(it->first == key || startsWith(it->first, prefix)) {
            it = m_own.values.erase(it);
            changedAny = true;
        } else
            ++it;
    }
    if(changedAny) markDirty();
}

StringList Settings::childKeys(const std::string& group) const {
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    std::set<std::string> keys;
    std::string prefix = group.empty() ? "" : group + "/";
    auto scan = [&](const IniFile& f) {
        for(auto& [k, _] : f.values) {
            if(!startsWith(k, prefix)) continue;
            std::string rest = k.substr(prefix.size());
            if(rest.find('/') == std::string::npos) keys.insert(rest);
        }
    };
    scan(m_own);
    for(auto& f : m_fallbacks) scan(f);
    return StringList(keys.begin(), keys.end());
}

StringList Settings::childGroups(const std::string& group) const {
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    std::set<std::string> groups;
    std::string prefix = group.empty() ? "" : group + "/";
    auto scan = [&](const IniFile& f) {
        for(auto& [k, _] : f.values) {
            if(!startsWith(k, prefix)) continue;
            std::string rest = k.substr(prefix.size());
            size_t s = rest.find('/');
            if(s != std::string::npos) groups.insert(rest.substr(0, s));
        }
    };
    scan(m_own);
    for(auto& f : m_fallbacks) scan(f);
    return StringList(groups.begin(), groups.end());
}

StringList Settings::allKeys(const std::string& group) const {
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    std::set<std::string> keys;
    std::string prefix = group.empty() ? "" : group + "/";
    auto scan = [&](const IniFile& f) {
        for(auto& [k, _] : f.values)
            if(startsWith(k, prefix)) keys.insert(k.substr(prefix.size()));
    };
    scan(m_own);
    for(auto& f : m_fallbacks) scan(f);
    return StringList(keys.begin(), keys.end());
}

std::vector<StringMap> Settings::readArray(const std::string& group) const {
    std::vector<StringMap> out;
    long long size = getInt(group + "/size", 0);
    for(long long i = 1; i <= size; i++) {
        StringMap m;
        std::string g = group + "/" + std::to_string(i);
        for(auto& k : childKeys(g)) m[k] = get(g + "/" + k);
        out.push_back(m);
    }
    // Arrays without a size key (e.g. hand-written WebServices sections)
    if(size == 0) {
        StringList groups = childGroups(group);
        std::sort(groups.begin(), groups.end(), [](const std::string& a, const std::string& b) { return toInt(a) < toInt(b); });
        for(auto& g : groups) {
            bool ok = false;
            toInt(g, 0, &ok);
            if(!ok) continue;
            StringMap m;
            for(auto& k : childKeys(group + "/" + g)) m[k] = get(group + "/" + g + "/" + k);
            out.push_back(m);
        }
    }
    return out;
}

void Settings::writeArray(const std::string& group, const std::vector<StringMap>& items) {
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    remove(group);
    for(size_t i = 0; i < items.size(); i++)
        for(auto& [k, v] : items[i]) set(group + "/" + std::to_string(i + 1) + "/" + k, v);
    setInt(group + "/size", (long long)items.size());
}

void Settings::renameGroup(const std::string& from, const std::string& to) {
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    std::string prefix = from + "/";
    std::vector<std::pair<std::string, IniValue>> moved;
    for(auto it = m_own.values.begin(); it != m_own.values.end();) {
        if(startsWith(it->first, prefix)) {
            moved.emplace_back(to + "/" + it->first.substr(prefix.size()), it->second);
            it = m_own.values.erase(it);
        } else
            ++it;
    }
    if(moved.empty()) return;
    for(auto& [k, v] : moved) m_own.values[k] = v;
    markDirty();
}

}  // namespace bg
