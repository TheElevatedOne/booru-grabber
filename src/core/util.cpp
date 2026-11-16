#include "core/util.hpp"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstring>

#include "html-entities.h"

namespace bg {

// ---- strings ---------------------------------------------------------------

StringList split(const std::string& s, char sep, bool skipEmpty) {
    StringList out;
    size_t start = 0;
    for(size_t i = 0; i <= s.size(); i++) {
        if(i == s.size() || s[i] == sep) {
            if(i > start || !skipEmpty) out.emplace_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

StringList split(const std::string& s, const std::string& sep, bool skipEmpty) {
    StringList out;
    if(sep.empty()) {
        out.push_back(s);
        return out;
    }
    size_t start = 0, pos;
    while((pos = s.find(sep, start)) != std::string::npos) {
        if(pos > start || !skipEmpty) out.emplace_back(s.substr(start, pos - start));
        start = pos + sep.size();
    }
    if(start < s.size() || !skipEmpty) out.emplace_back(s.substr(start));
    return out;
}

StringList splitAny(const std::string& s, const std::string& seps, bool skipEmpty) {
    StringList out;
    size_t start = 0;
    for(size_t i = 0; i <= s.size(); i++) {
        if(i == s.size() || seps.find(s[i]) != std::string::npos) {
            if(i > start || !skipEmpty) out.emplace_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

std::string join(const StringList& l, const std::string& sep) {
    std::string out;
    for(size_t i = 0; i < l.size(); i++) {
        if(i) out += sep;
        out += l[i];
    }
    return out;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while(b < e && g_ascii_isspace(s[b])) b++;
    while(e > b && g_ascii_isspace(s[e - 1])) e--;
    return s.substr(b, e - b);
}

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    if(from.empty()) return s;
    size_t pos = 0;
    while((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

bool startsWith(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0 && s.size() >= p.size(); }
bool endsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}
bool contains(const std::string& s, const std::string& n) { return s.find(n) != std::string::npos; }
bool contains(const StringList& l, const std::string& v) { return std::find(l.begin(), l.end(), v) != l.end(); }
bool containsCi(const StringList& l, const std::string& v) {
    for(auto& x : l)
        if(iequals(x, v)) return true;
    return false;
}

std::string toLower(const std::string& s) {
    gchar* r = g_utf8_strdown(s.c_str(), (gssize)s.size());
    std::string out = r ? r : "";
    g_free(r);
    return out;
}

std::string toUpper(const std::string& s) {
    gchar* r = g_utf8_strup(s.c_str(), (gssize)s.size());
    std::string out = r ? r : "";
    g_free(r);
    return out;
}

bool iequals(const std::string& a, const std::string& b) {
    if(a.size() == b.size() && g_ascii_strcasecmp(a.c_str(), b.c_str()) == 0) return true;
    gchar* fa = g_utf8_casefold(a.c_str(), (gssize)a.size());
    gchar* fb = g_utf8_casefold(b.c_str(), (gssize)b.size());
    bool r = g_strcmp0(fa, fb) == 0;
    g_free(fa);
    g_free(fb);
    return r;
}

std::string upperFirst(const std::string& s) {
    if(s.empty()) return s;
    const char* next = g_utf8_next_char(s.c_str());
    size_t n = (size_t)(next - s.c_str());
    return toUpper(s.substr(0, n)) + s.substr(n);
}

long long toInt(const std::string& s, long long def, bool* ok) {
    std::string t = trim(s);
    if(ok) *ok = false;
    if(t.empty()) return def;
    char* end = nullptr;
    errno = 0;
    long long v = g_ascii_strtoll(t.c_str(), &end, 10);
    if(errno || !end || *end) return def;
    if(ok) *ok = true;
    return v;
}

unsigned long long toUInt(const std::string& s, unsigned long long def) {
    std::string t = trim(s);
    if(t.empty() || t[0] == '-') return def;
    char* end = nullptr;
    errno = 0;
    unsigned long long v = g_ascii_strtoull(t.c_str(), &end, 10);
    if(errno || !end || *end) return def;
    return v;
}

double toDouble(const std::string& s, double def, bool* ok) {
    std::string t = trim(s);
    if(ok) *ok = false;
    if(t.empty()) return def;
    char* end = nullptr;
    double v = g_ascii_strtod(t.c_str(), &end);
    if(!end || *end) return def;
    if(ok) *ok = true;
    return v;
}

bool toBool(const std::string& s, bool def) {
    std::string t = toLower(trim(s));
    if(t == "true" || t == "1" || t == "yes" || t == "on") return true;
    if(t == "false" || t == "0" || t == "no" || t == "off") return false;
    return def;
}

std::string fromDouble(double v, int precision) {
    char buf[G_ASCII_DTOSTR_BUF_SIZE];
    if(precision < 0) {
        if(std::floor(v) == v && std::fabs(v) < 1e15) return std::to_string((long long)v);
        g_ascii_dtostr(buf, sizeof buf, v);
        return buf;
    }
    std::string fmt = "%." + std::to_string(precision) + "f";
    g_ascii_formatd(buf, sizeof buf, fmt.c_str(), v);
    return buf;
}

std::string leftPad(const std::string& s, size_t width, char c) {
    size_t len = utf8Length(s);
    if(len >= width) return s;
    return std::string(width - len, c) + s;
}

size_t utf8Length(const std::string& s) {
    if(!g_utf8_validate(s.c_str(), (gssize)s.size(), nullptr)) return s.size();
    return (size_t)g_utf8_strlen(s.c_str(), (gssize)s.size());
}

std::string utf8Left(const std::string& s, size_t chars) {
    if(!g_utf8_validate(s.c_str(), (gssize)s.size(), nullptr)) return s.substr(0, chars);
    const char* p = s.c_str();
    const char* end = p + s.size();
    while(chars > 0 && p < end) {
        p = g_utf8_next_char(p);
        chars--;
    }
    return std::string(s.c_str(), (size_t)(p - s.c_str()));
}

std::string utf8TruncateBytes(const std::string& s, size_t maxBytes) {
    if(s.size() <= maxBytes) return s;
    size_t cut = maxBytes;
    while(cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) cut--;
    return s.substr(0, cut);
}

std::string htmlEscape(const std::string& s) {
    gchar* e = g_markup_escape_text(s.c_str(), (gssize)s.size());
    std::string r = e;
    g_free(e);
    return r;
}

std::string decodeHtmlEntities(const std::string& s) {
    if(s.find('&') == std::string::npos) return s;
    std::vector<char> dest(s.size() + 1);
    decode_html_entities_utf8(dest.data(), s.c_str());
    return std::string(dest.data());
}

std::string stripTags(const std::string& s) {
    static const Regex rx("<[^>]*>");
    return rx.replace(s, "");
}

void removeDuplicates(StringList& l) {
    StringList out;
    for(auto& s : l)
        if(!contains(out, s)) out.push_back(s);
    l = std::move(out);
}

void removeAll(StringList& l, const std::string& v) { l.erase(std::remove(l.begin(), l.end(), v), l.end()); }

int naturalCompare(const std::string& a, const std::string& b) {
    gchar* ka = g_utf8_collate_key_for_filename(a.c_str(), (gssize)a.size());
    gchar* kb = g_utf8_collate_key_for_filename(b.c_str(), (gssize)b.size());
    int r = strcmp(ka, kb);
    g_free(ka);
    g_free(kb);
    return r;
}

std::string formatFilesize(double size) {
    static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int power = 0;
    while(size >= 1024 && power < 4) {
        size /= 1024;
        power++;
    }
    double round = size > 100 ? 1 : (size >= 10 ? 10 : 100);
    double rounded = std::round(size * round) / round;
    return fromDouble(rounded) + " " + units[power];
}

long long parseFileSize(const std::string& str) {
    static const Regex rx("^(\\d+(?:\\.\\d+)?)\\s*([a-zA-Z]+)$");
    auto m = rx.match(trim(str));
    if(m) {
        double val = toDouble(m->groups[1]);
        std::string unit = toLower(m->groups[2]);
        if(unit == "gb" || unit == "gib" || unit == "g") return (long long)std::llround(val * 1024 * 1024 * 1024);
        if(unit == "mb" || unit == "mib" || unit == "m") return (long long)std::llround(val * 1024 * 1024);
        if(unit == "kb" || unit == "kib" || unit == "k") return (long long)std::llround(val * 1024);
        return (long long)std::llround(val);
    }
    return (long long)std::llround(toDouble(str));
}

std::string sprintf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    gchar* s = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    std::string r = s;
    g_free(s);
    return r;
}

// Splits a command line, supporting both quote styles and backslash escapes.
// Triple quotes produce a literal quote character, like the original Grabber.
StringList splitCommand(const std::string& command) {
    StringList args;
    std::string tmp;
    int quoteCount = 0;
    char lastQuote = 0;
    bool inQuote = false, inEscape = false;
    for(char c : command) {
        if(c == '\\' && !inEscape) {
            inEscape = true;
            continue;
        }
        if(inEscape) {
            inEscape = false;
            tmp += c;
            continue;
        }
        if((c == '"' || c == '\'') && (c == lastQuote || lastQuote == 0)) {
            ++quoteCount;
            lastQuote = c;
            if(quoteCount == 3) {
                quoteCount = 0;
                tmp += c;
            }
            continue;
        }
        if(quoteCount) {
            if(quoteCount == 1) {
                inQuote = !inQuote;
                if(!inQuote) lastQuote = 0;
            }
            quoteCount = 0;
        }
        if(!inQuote && g_ascii_isspace(c)) {
            if(!tmp.empty()) {
                args.push_back(tmp);
                tmp.clear();
            }
        } else {
            tmp += c;
        }
    }
    if(!tmp.empty()) args.push_back(tmp);
    return args;
}

// ---- Regex -----------------------------------------------------------------

Regex::Regex(const std::string& pattern, bool ci, bool dotAll, bool multiline) : m_pattern(pattern) {
    int flags = G_REGEX_OPTIMIZE;
    if(ci) flags |= G_REGEX_CASELESS;
    if(dotAll) flags |= G_REGEX_DOTALL;
    if(multiline) flags |= G_REGEX_MULTILINE;
    GError* err = nullptr;
    GRegex* re = g_regex_new(pattern.c_str(), (GRegexCompileFlags)flags, (GRegexMatchFlags)0, &err);
    if(!re) {
        m_error = err ? err->message : "invalid regular expression";
        if(err) g_error_free(err);
        return;
    }
    m_re = std::shared_ptr<GRegex>(re, g_regex_unref);
}

std::string Regex::Match::group(const std::string& name) const {
    auto it = named.find(name);
    return it == named.end() ? std::string() : it->second;
}

bool Regex::matches(const std::string& s) const {
    if(!m_re) return false;
    return g_regex_match_full(m_re.get(), s.c_str(), (gssize)s.size(), 0, (GRegexMatchFlags)0, nullptr, nullptr);
}

static Regex::Match buildMatch(GMatchInfo* mi, const StringList& names) {
    Regex::Match m;
    int n = g_match_info_get_match_count(mi);
    for(int i = 0; i < n; i++) {
        int s = -1, e = -1;
        g_match_info_fetch_pos(mi, i, &s, &e);
        gchar* g = g_match_info_fetch(mi, i);
        m.groups.emplace_back(g ? g : "");
        m.offsets.emplace_back(s, e);
        g_free(g);
    }
    for(auto& name : names) {
        gchar* g = g_match_info_fetch_named(mi, name.c_str());
        if(g && *g) m.named[name] = g;
        g_free(g);
    }
    return m;
}

std::optional<Regex::Match> Regex::match(const std::string& s, int start) const {
    if(!m_re) return std::nullopt;
    GMatchInfo* mi = nullptr;
    std::optional<Match> r;
    if(g_regex_match_full(m_re.get(), s.c_str(), (gssize)s.size(), start, (GRegexMatchFlags)0, &mi, nullptr))
        r = buildMatch(mi, namedGroups());
    g_match_info_free(mi);
    return r;
}

std::vector<Regex::Match> Regex::matchAll(const std::string& s) const {
    std::vector<Match> out;
    if(!m_re) return out;
    StringList names = namedGroups();
    GMatchInfo* mi = nullptr;
    GError* err = nullptr;
    g_regex_match_full(m_re.get(), s.c_str(), (gssize)s.size(), 0, (GRegexMatchFlags)0, &mi, &err);
    while(!err && g_match_info_matches(mi)) {
        out.push_back(buildMatch(mi, names));
        if(!g_match_info_next(mi, &err)) break;
    }
    if(err) {
        g_warning("Regular expression error after %zu matches: %s", out.size(), err->message);
        g_error_free(err);
    }
    g_match_info_free(mi);
    return out;
}

std::string Regex::replace(const std::string& s, const std::string& replacement) const {
    if(!m_re) return s;
    gchar* r = g_regex_replace(m_re.get(), s.c_str(), (gssize)s.size(), 0, replacement.c_str(), (GRegexMatchFlags)0, nullptr);
    if(!r) return s;
    std::string out = r;
    g_free(r);
    return out;
}

// GRegex has no API to list named groups, so scan the pattern for (?<name>, (?P<name> and (?'name'.
StringList Regex::namedGroups() const {
    StringList names;
    const std::string& p = m_pattern;
    bool inClass = false;
    for(size_t i = 0; i < p.size(); i++) {
        char c = p[i];
        if(c == '\\') {
            i++;
            continue;
        }
        if(inClass) {
            if(c == ']') inClass = false;
            continue;
        }
        if(c == '[') {
            inClass = true;
            continue;
        }
        if(c != '(' || i + 2 >= p.size() || p[i + 1] != '?') continue;
        size_t j = i + 2;
        char close = '>';
        if(p[j] == 'P' && j + 1 < p.size() && p[j + 1] == '<')
            j += 2;
        else if(p[j] == '<' && j + 1 < p.size() && p[j + 1] != '=' && p[j + 1] != '!')
            j += 1;
        else if(p[j] == '\'') {
            j += 1;
            close = '\'';
        } else
            continue;
        size_t end = p.find(close, j);
        if(end == std::string::npos) continue;
        std::string name = p.substr(j, end - j);
        if(!name.empty() && !contains(names, name)) names.push_back(name);
    }
    return names;
}

std::string Regex::escape(const std::string& s) {
    gchar* e = g_regex_escape_string(s.c_str(), (gint)s.size());
    std::string r = e;
    g_free(e);
    return r;
}

Regex Regex::fromWildcard(const std::string& w, bool ci) {
    std::string rx = "^";
    for(size_t i = 0; i < w.size(); i++) {
        char c = w[i];
        if(c == '*')
            rx += ".*";
        else if(c == '?')
            rx += '.';
        else if(c == '[') {
            size_t end = w.find(']', i + 1);
            if(end == std::string::npos) {
                rx += "\\[";
            } else {
                std::string cls = w.substr(i + 1, end - i - 1);
                if(!cls.empty() && cls[0] == '!') cls[0] = '^';
                rx += "[" + cls + "]";
                i = end;
            }
        } else
            rx += escape(std::string(1, c));
    }
    rx += "$";
    return Regex(rx, ci, true);
}

// ---- files & paths ---------------------------------------------------------

std::string buildPath(const std::string& a, const std::string& b) {
    if(a.empty()) return b;
    if(b.empty()) return a;
    gchar* p = g_build_filename(a.c_str(), b.c_str(), nullptr);
    std::string r = p;
    g_free(p);
    return r;
}
std::string buildPath(const std::string& a, const std::string& b, const std::string& c) { return buildPath(buildPath(a, b), c); }

std::string dirName(const std::string& path) {
    gchar* d = g_path_get_dirname(path.c_str());
    std::string r = d;
    g_free(d);
    return r;
}

std::string baseName(const std::string& path) {
    if(path.empty()) return "";
    gchar* b = g_path_get_basename(path.c_str());
    std::string r = b;
    g_free(b);
    return r;
}

std::string completeBaseName(const std::string& path) {
    std::string b = baseName(path);
    size_t dot = b.rfind('.');
    return dot == std::string::npos || dot == 0 ? b : b.substr(0, dot);
}

std::string fileSuffix(const std::string& path) {
    std::string b = baseName(path);
    size_t dot = b.rfind('.');
    return dot == std::string::npos || dot == 0 ? "" : b.substr(dot + 1);
}

std::string cleanPath(const std::string& path) {
    if(path.empty()) return path;
    gchar* c = g_canonicalize_filename(path.c_str(), g_path_is_absolute(path.c_str()) ? nullptr : "/");
    std::string r = c;
    g_free(c);
    if(!g_path_is_absolute(path.c_str()) && startsWith(r, "/")) r = r.substr(1);
    return r;
}

std::string absolutePath(const std::string& path) {
    gchar* c = g_canonicalize_filename(expandHome(path).c_str(), nullptr);
    std::string r = c;
    g_free(c);
    return r;
}

std::string expandHome(const std::string& path) {
    if(path == "~") return g_get_home_dir();
    if(startsWith(path, "~/")) return buildPath(g_get_home_dir(), path.substr(2));
    return path;
}

bool fileExists(const std::string& path) { return !path.empty() && g_file_test(path.c_str(), G_FILE_TEST_EXISTS); }
bool isDir(const std::string& path) { return !path.empty() && g_file_test(path.c_str(), G_FILE_TEST_IS_DIR); }
bool isWritable(const std::string& path) { return !path.empty() && g_access(path.c_str(), W_OK) == 0; }

bool mkpath(const std::string& dir) {
    if(dir.empty()) return false;
    return g_mkdir_with_parents(dir.c_str(), 0755) == 0;
}

bool ensureParent(const std::string& file) {
    std::string d = dirName(file);
    return d == "." || isDir(d) || mkpath(d);
}

bool removeFile(const std::string& path) { return g_remove(path.c_str()) == 0; }

bool renameFile(const std::string& from, const std::string& to) {
    if(g_rename(from.c_str(), to.c_str()) == 0) return true;
    if(errno != EXDEV) return false;
    return moveFile(from, to);
}

bool copyFile(const std::string& from, const std::string& to, bool overwrite) {
    GFile* src = g_file_new_for_path(from.c_str());
    GFile* dst = g_file_new_for_path(to.c_str());
    int flags = G_FILE_COPY_ALL_METADATA;
    if(overwrite) flags |= G_FILE_COPY_OVERWRITE;
    bool ok = g_file_copy(src, dst, (GFileCopyFlags)flags, nullptr, nullptr, nullptr, nullptr);
    g_object_unref(src);
    g_object_unref(dst);
    return ok;
}

bool moveFile(const std::string& from, const std::string& to) {
    GFile* src = g_file_new_for_path(from.c_str());
    GFile* dst = g_file_new_for_path(to.c_str());
    bool ok = g_file_move(src, dst, G_FILE_COPY_ALL_METADATA, nullptr, nullptr, nullptr, nullptr);
    g_object_unref(src);
    g_object_unref(dst);
    return ok;
}

int64_t fileSize(const std::string& path) {
    GStatBuf st;
    if(g_stat(path.c_str(), &st) != 0) return -1;
    return (int64_t)st.st_size;
}

int64_t fileMTime(const std::string& path) {
    GStatBuf st;
    if(g_stat(path.c_str(), &st) != 0) return 0;
    return (int64_t)st.st_mtime;
}

bool setFileMTime(const std::string& path, int64_t secs) {
    struct utimbuf tb;
    tb.modtime = (time_t)secs;
    tb.actime = time(nullptr);
    return utime(path.c_str(), &tb) == 0;
}

std::optional<std::string> readFile(const std::string& path) {
    gchar* data = nullptr;
    gsize len = 0;
    if(!g_file_get_contents(path.c_str(), &data, &len, nullptr)) return std::nullopt;
    std::string r(data, len);
    g_free(data);
    return r;
}

bool writeFile(const std::string& path, const std::string& data) {
    if(!ensureParent(path)) return false;
    return g_file_set_contents_full(path.c_str(), data.data(), (gssize)data.size(),
                                    G_FILE_SET_CONTENTS_CONSISTENT, 0644, nullptr);
}

bool appendFile(const std::string& path, const std::string& data) {
    if(!ensureParent(path)) return false;
    FILE* f = g_fopen(path.c_str(), "ab");
    if(!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    return ok;
}

StringList readLines(const std::string& path, bool skipEmpty) {
    StringList out;
    auto d = readFile(path);
    if(!d) return out;
    for(auto& line : split(*d, '\n')) {
        std::string t = trim(line);
        if(t.empty() && skipEmpty) continue;
        out.push_back(t);
    }
    return out;
}

StringList listDir(const std::string& dir, bool dirs, bool files) {
    StringList out;
    GDir* d = g_dir_open(dir.c_str(), 0, nullptr);
    if(!d) return out;
    const gchar* name;
    while((name = g_dir_read_name(d))) {
        std::string full = buildPath(dir, name);
        bool isD = isDir(full);
        if((isD && dirs) || (!isD && files)) out.emplace_back(name);
    }
    g_dir_close(d);
    std::sort(out.begin(), out.end());
    return out;
}

std::string fileMd5(const std::string& path) {
    FILE* f = g_fopen(path.c_str(), "rb");
    if(!f) return "";
    GChecksum* ck = g_checksum_new(G_CHECKSUM_MD5);
    std::vector<unsigned char> buf(1 << 16);
    size_t n;
    while((n = fread(buf.data(), 1, buf.size(), f)) > 0) g_checksum_update(ck, buf.data(), (gssize)n);
    fclose(f);
    std::string r = g_checksum_get_string(ck);
    g_checksum_free(ck);
    return r;
}

std::string headerExtension(const std::string& path) {
    FILE* f = g_fopen(path.c_str(), "rb");
    if(!f) return "";
    char buf[16] = {0};
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    return headerExtensionFromData(std::string(buf, n));
}

std::string headerExtensionFromData(const std::string& d) {
    auto at = [&](size_t off, const std::string& sig) { return d.size() >= off + sig.size() && d.compare(off, sig.size(), sig) == 0; };
    if(at(0, "GIF87a") || at(0, "GIF89a")) return "gif";
    if(at(0, "\x89PNG\r\n\x1a\n")) return "png";
    if(at(0, "\xff\xd8\xff")) return "jpg";
    if(at(0, "RIFF") && at(8, "WEBP")) return "webp";
    if(at(0, "BM")) return "bmp";
    if(at(0, "\x1a\x45\xdf\xa3")) return "webm";
    if(at(4, "ftypavif") || at(4, "ftypavis")) return "avif";
    if(at(4, "ftypheic") || at(4, "ftypheix") || at(4, "ftypmif1")) return "heic";
    if(at(4, "ftyp3gp5") || at(4, "ftypMSNV") || at(4, "ftypisom") || at(4, "ftypmp42") || at(4, "ftypmp41") || at(4, "ftypiso5"))
        return "mp4";
    if(at(0, "\xff\x0a") || at(0, std::string("\x00\x00\x00\x0cJXL ", 8))) return "jxl";
    if(at(0, "FWS") || at(0, "CWS") || at(0, "ZWS")) return "swf";
    if(at(0, std::string("FLV\x01", 4))) return "flv";
    if(at(0, std::string("\x00\x00\x01\x00", 4))) return "ico";
    if(at(0, "PK\x03\x04")) return "zip";
    return "";
}

bool createLink(const std::string& from, const std::string& to, const std::string& type) {
    if(type == "link" || type == "symlink") return symlink(from.c_str(), to.c_str()) == 0;
    if(type == "hardlink") return link(from.c_str(), to.c_str()) == 0;
    return false;
}

std::string tempDir() {
    const char* runtime = g_get_user_runtime_dir();
    std::string base = runtime && *runtime ? runtime : g_get_tmp_dir();
    std::string dir = buildPath(base, "booru-grabber");
    mkpath(dir);
    return dir;
}

// ---- hashing / encoding ----------------------------------------------------

static std::string checksumHex(GChecksumType t, const std::string& data) {
    gchar* s = g_compute_checksum_for_data(t, (const guchar*)data.data(), data.size());
    std::string r = s;
    g_free(s);
    return r;
}
std::string md5Hex(const std::string& d) { return checksumHex(G_CHECKSUM_MD5, d); }
std::string sha1Hex(const std::string& d) { return checksumHex(G_CHECKSUM_SHA1, d); }
std::string sha256Raw(const std::string& data) {
    GChecksum* ck = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(ck, (const guchar*)data.data(), (gssize)data.size());
    guint8 buf[32];
    gsize len = sizeof buf;
    g_checksum_get_digest(ck, buf, &len);
    g_checksum_free(ck);
    return std::string((const char*)buf, len);
}

std::string base64Encode(const std::string& d) {
    gchar* e = g_base64_encode((const guchar*)d.data(), d.size());
    std::string r = e;
    g_free(e);
    return r;
}

std::string base64Decode(const std::string& d) {
    gsize len = 0;
    guchar* r = g_base64_decode(d.c_str(), &len);
    std::string out((const char*)r, len);
    g_free(r);
    return out;
}

std::string base64UrlEncode(const std::string& d) {
    std::string s = base64Encode(d);
    for(auto& c : s) {
        if(c == '+') c = '-';
        else if(c == '/') c = '_';
    }
    while(!s.empty() && s.back() == '=') s.pop_back();
    return s;
}

std::string base64UrlDecode(const std::string& d) {
    std::string s = d;
    for(auto& c : s) {
        if(c == '-') c = '+';
        else if(c == '_') c = '/';
    }
    while(s.size() % 4) s += '=';
    return base64Decode(s);
}

std::string hexEncode(const std::string& d) {
    static const char* hex = "0123456789abcdef";
    std::string r;
    r.reserve(d.size() * 2);
    for(unsigned char c : d) {
        r += hex[c >> 4];
        r += hex[c & 15];
    }
    return r;
}

std::string randomBytes(size_t n) {
    std::string r(n, '\0');
    for(size_t i = 0; i < n; i++) r[i] = (char)g_random_int_range(0, 256);
    return r;
}

std::string uuid() {
    gchar* u = g_uuid_string_random();
    std::string r = u;
    g_free(u);
    return r;
}

// ---- URLs ------------------------------------------------------------------

std::string urlEncode(const std::string& s, const char* reserved) {
    gchar* e = g_uri_escape_string(s.c_str(), reserved, FALSE);
    std::string r = e;
    g_free(e);
    return r;
}

std::string urlDecode(const std::string& s) {
    gchar* d = g_uri_unescape_string(s.c_str(), nullptr);
    if(!d) return s;
    std::string r = d;
    g_free(d);
    return r;
}

static GUri* parseUri(const std::string& url) {
    return g_uri_parse(url.c_str(), (GUriFlags)(G_URI_FLAGS_ENCODED | G_URI_FLAGS_PARSE_RELAXED), nullptr);
}

std::string urlScheme(const std::string& url) {
    size_t p = url.find("://");
    return p == std::string::npos ? "" : toLower(url.substr(0, p));
}

std::string urlHost(const std::string& url) {
    GUri* u = parseUri(url);
    if(!u) return "";
    std::string r = g_uri_get_host(u) ? g_uri_get_host(u) : "";
    g_uri_unref(u);
    return r;
}

std::string urlPath(const std::string& url) {
    GUri* u = parseUri(url);
    if(!u) {
        size_t q = url.find_first_of("?#");
        return q == std::string::npos ? url : url.substr(0, q);
    }
    std::string r = g_uri_get_path(u);
    g_uri_unref(u);
    return r;
}

std::string urlQuery(const std::string& url) {
    GUri* u = parseUri(url);
    if(!u) return "";
    std::string r = g_uri_get_query(u) ? g_uri_get_query(u) : "";
    g_uri_unref(u);
    return r;
}

std::string urlFileName(const std::string& url) {
    std::string p = urlPath(url);
    size_t s = p.rfind('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

std::string urlResolve(const std::string& base, const std::string& rel) {
    if(base.empty()) return rel;
    GError* err = nullptr;
    gchar* r = g_uri_resolve_relative(base.c_str(), rel.c_str(), (GUriFlags)(G_URI_FLAGS_ENCODED | G_URI_FLAGS_PARSE_RELAXED), &err);
    if(!r) {
        if(err) g_error_free(err);
        return rel;
    }
    std::string out = r;
    g_free(r);
    return out;
}

std::string urlSetScheme(const std::string& url, const std::string& scheme) {
    size_t p = url.find("://");
    if(p == std::string::npos) return url;
    return scheme + url.substr(p);
}

std::string urlQueryValue(const std::string& url, const std::string& key) {
    std::string q = urlQuery(url);
    for(auto& part : split(q, '&', true)) {
        size_t eq = part.find('=');
        std::string k = urlDecode(part.substr(0, eq));
        if(k == key) return eq == std::string::npos ? "" : urlDecode(replaceAll(part.substr(eq + 1), "+", " "));
    }
    return "";
}

bool isUrl(const std::string& s) {
    static const Regex rx("^https?://[^\\s/$.?#].[^\\s]*$");
    return rx.matches(s);
}

static std::string extensionOf(const std::string& s) {
    size_t dot = s.rfind('.');
    if(dot == std::string::npos) return "";
    std::string ext = s.substr(dot + 1);
    size_t colon = ext.find(':');
    return colon == std::string::npos ? ext : ext.substr(0, colon);
}

std::string getExtension(const std::string& url) {
    std::string ext = extensionOf(urlFileName(url));
    if(ext.empty() || ext == "php" || ext == "html") {
        size_t slash = url.rfind('/');
        std::string last = slash == std::string::npos ? url : url.substr(slash + 1);
        return extensionOf(last);
    }
    return ext;
}

std::string setExtension(const std::string& path, const std::string& ext) {
    size_t slash = path.rfind('/');
    size_t start = slash == std::string::npos ? 0 : slash + 1;
    size_t dot = path.rfind('.');
    if(dot == std::string::npos || dot < start) return path;
    return path.substr(0, dot) + (ext.empty() ? "" : "." + ext);
}

std::string urlSetExtension(const std::string& url, const std::string& ext) {
    size_t q = url.find_first_of("?#");
    std::string base = q == std::string::npos ? url : url.substr(0, q);
    std::string rest = q == std::string::npos ? "" : url.substr(q);
    size_t slash = base.rfind('/');
    size_t start = slash == std::string::npos ? 0 : slash + 1;
    size_t dot = base.rfind('.');
    if(dot == std::string::npos || dot < start) return url;
    std::string after = base.substr(dot + 1);
    size_t colon = after.find(':');
    std::string suffix = colon == std::string::npos ? "" : after.substr(colon);
    return base.substr(0, dot) + "." + ext + suffix + rest;
}

std::string removeCacheBuster(const std::string& url) {
    size_t q = url.find('?');
    if(q == std::string::npos) return url;
    std::string query = url.substr(q + 1);
    bool ok = false;
    toInt(query, 0, &ok);
    return ok ? url.substr(0, q) : url;
}

// ---- misc ------------------------------------------------------------------

int64_t nowMs() { return g_get_real_time() / 1000; }

void openUri(const std::string& uri) {
    std::string u = uri;
    if(!u.empty() && u[0] == '/') {
        gchar* fu = g_filename_to_uri(u.c_str(), nullptr, nullptr);
        if(fu) {
            u = fu;
            g_free(fu);
        }
    }
    GError* err = nullptr;
    if(!g_app_info_launch_default_for_uri(u.c_str(), nullptr, &err)) {
        g_warning("Cannot open %s: %s", u.c_str(), err ? err->message : "unknown error");
        if(err) g_error_free(err);
    }
}

namespace {
struct Deferred {
    std::function<void()> f;
};
gboolean runDeferred(gpointer p) {
    auto* d = static_cast<Deferred*>(p);
    d->f();
    return G_SOURCE_REMOVE;
}
void freeDeferred(gpointer p) { delete static_cast<Deferred*>(p); }
}  // namespace

void defer(std::function<void()> f) { g_idle_add_full(G_PRIORITY_DEFAULT, runDeferred, new Deferred{std::move(f)}, freeDeferred); }

guint after(unsigned ms, std::function<void()> f) {
    if(ms == 0) {
        return g_idle_add_full(G_PRIORITY_DEFAULT, runDeferred, new Deferred{std::move(f)}, freeDeferred);
    }
    return g_timeout_add_full(G_PRIORITY_DEFAULT, ms, runDeferred, new Deferred{std::move(f)}, freeDeferred);
}

}  // namespace bg
