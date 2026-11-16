// General helpers: strings, files, regular expressions, URLs, hashing.
// Everything in the core works on UTF-8 std::string.
#pragma once

#include <glib.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace bg {

using StringList = std::vector<std::string>;
using StringMap = std::map<std::string, std::string>;

// ---- strings ---------------------------------------------------------------
StringList split(const std::string& s, char sep, bool skipEmpty = false);
StringList split(const std::string& s, const std::string& sep, bool skipEmpty = false);
StringList splitAny(const std::string& s, const std::string& seps, bool skipEmpty = false);
std::string join(const StringList& l, const std::string& sep);
std::string trim(const std::string& s);
std::string replaceAll(std::string s, const std::string& from, const std::string& to);
bool startsWith(const std::string& s, const std::string& prefix);
bool endsWith(const std::string& s, const std::string& suffix);
bool contains(const std::string& s, const std::string& needle);
bool contains(const StringList& l, const std::string& v);
bool containsCi(const StringList& l, const std::string& v);
std::string toLower(const std::string& s);  // UTF-8 aware
std::string toUpper(const std::string& s);
bool iequals(const std::string& a, const std::string& b);
std::string upperFirst(const std::string& s);
long long toInt(const std::string& s, long long def = 0, bool* ok = nullptr);
unsigned long long toUInt(const std::string& s, unsigned long long def = 0);
double toDouble(const std::string& s, double def = 0, bool* ok = nullptr);
bool toBool(const std::string& s, bool def = false);
std::string fromDouble(double v, int precision = -1);  // -1: shortest representation
std::string leftPad(const std::string& s, size_t width, char c = '0');
std::string utf8Left(const std::string& s, size_t chars);
size_t utf8Length(const std::string& s);
std::string utf8TruncateBytes(const std::string& s, size_t maxBytes);
std::string htmlEscape(const std::string& s);
std::string decodeHtmlEntities(const std::string& s);
std::string stripTags(const std::string& s);
void removeDuplicates(StringList& l);
void removeAll(StringList& l, const std::string& v);
int naturalCompare(const std::string& a, const std::string& b);
std::string formatFilesize(double bytes);
long long parseFileSize(const std::string& s);
std::string sprintf(const char* fmt, ...) G_GNUC_PRINTF(1, 2);
StringList splitCommand(const std::string& command);

// ---- regular expressions (GRegex / PCRE2) ---------------------------------
class Regex {
public:
    struct Match {
        std::vector<std::string> groups;          // 0 = whole match
        std::vector<std::pair<int, int>> offsets;  // byte offsets per group, -1 if unset
        std::map<std::string, std::string> named;
        const std::string& operator[](size_t i) const { return groups[i]; }
        std::string group(const std::string& name) const;
    };
    Regex() = default;
    explicit Regex(const std::string& pattern, bool caseInsensitive = false, bool dotAll = false,
                   bool multiline = false);
    Regex(const Regex&) = default;
    Regex& operator=(const Regex&) = default;
    bool valid() const { return m_re != nullptr; }
    const std::string& error() const { return m_error; }
    const std::string& pattern() const { return m_pattern; }
    bool matches(const std::string& s) const;
    std::optional<Match> match(const std::string& s, int start = 0) const;
    std::vector<Match> matchAll(const std::string& s) const;
    std::string replace(const std::string& s, const std::string& replacement) const;  // \1 style backrefs
    StringList namedGroups() const;
    static std::string escape(const std::string& s);
    static Regex fromWildcard(const std::string& wildcard, bool caseInsensitive = true);

private:
    std::shared_ptr<GRegex> m_re;
    std::string m_pattern;
    std::string m_error;
};

// ---- files & paths ---------------------------------------------------------
std::string buildPath(const std::string& a, const std::string& b);
std::string buildPath(const std::string& a, const std::string& b, const std::string& c);
std::string dirName(const std::string& path);
std::string baseName(const std::string& path);
std::string completeBaseName(const std::string& path);  // file name without the last suffix
std::string fileSuffix(const std::string& path);         // last suffix without dot
std::string cleanPath(const std::string& path);
std::string absolutePath(const std::string& path);
std::string expandHome(const std::string& path);
bool fileExists(const std::string& path);
bool isDir(const std::string& path);
bool isWritable(const std::string& path);
bool mkpath(const std::string& dir);
bool ensureParent(const std::string& file);
bool removeFile(const std::string& path);
bool renameFile(const std::string& from, const std::string& to);  // falls back to copy+delete across filesystems
bool copyFile(const std::string& from, const std::string& to, bool overwrite = false);
bool moveFile(const std::string& from, const std::string& to);
int64_t fileSize(const std::string& path);
int64_t fileMTime(const std::string& path);  // unix seconds
bool setFileMTime(const std::string& path, int64_t unixSeconds);
std::optional<std::string> readFile(const std::string& path);
bool writeFile(const std::string& path, const std::string& data);  // atomic, creates parents
bool appendFile(const std::string& path, const std::string& data);
StringList readLines(const std::string& path, bool skipEmpty = true);
StringList listDir(const std::string& dir, bool dirs, bool files);
std::string fileMd5(const std::string& path);
std::string headerExtension(const std::string& path);
std::string headerExtensionFromData(const std::string& data);
bool createLink(const std::string& from, const std::string& to, const std::string& type);
std::string tempDir();

// ---- hashing / encoding ----------------------------------------------------
std::string md5Hex(const std::string& data);
std::string sha1Hex(const std::string& data);
std::string sha256Raw(const std::string& data);
std::string base64Encode(const std::string& data);
std::string base64Decode(const std::string& data);
std::string base64UrlEncode(const std::string& data);
std::string base64UrlDecode(const std::string& data);
std::string hexEncode(const std::string& data);
std::string randomBytes(size_t n);
std::string uuid();

// ---- URLs ------------------------------------------------------------------
std::string urlEncode(const std::string& s, const char* reserved = nullptr);
std::string urlDecode(const std::string& s);
std::string urlScheme(const std::string& url);
std::string urlHost(const std::string& url);
std::string urlPath(const std::string& url);
std::string urlQuery(const std::string& url);
std::string urlFileName(const std::string& url);
std::string urlResolve(const std::string& base, const std::string& relative);
std::string urlSetScheme(const std::string& url, const std::string& scheme);
std::string urlQueryValue(const std::string& url, const std::string& key);
bool isUrl(const std::string& s);
std::string getExtension(const std::string& url);  // extension of a URL's file name
std::string setExtension(const std::string& path, const std::string& ext);
std::string urlSetExtension(const std::string& url, const std::string& ext);
std::string removeCacheBuster(const std::string& url);

// ---- misc ------------------------------------------------------------------
int64_t nowMs();
void openUri(const std::string& uri);

// Lightweight multi-listener signal.
template <typename... A>
class Signal {
public:
    using Fn = std::function<void(A...)>;
    int connect(Fn f) {
        m_slots.emplace_back(++m_next, std::move(f));
        return m_next;
    }
    void disconnect(int id) {
        for(auto it = m_slots.begin(); it != m_slots.end(); ++it)
            if(it->first == id) {
                m_slots.erase(it);
                return;
            }
    }
    void clear() { m_slots.clear(); }
    void emit(A... a) {
        auto copy = m_slots;
        for(auto& s : copy) s.second(a...);
    }
    bool empty() const { return m_slots.empty(); }

private:
    std::vector<std::pair<int, Fn>> m_slots;
    int m_next = 0;
};

// Run a callable on the next main loop iteration.
void defer(std::function<void()> f);
// Run a callable after ms milliseconds; returns the GSource id.
guint after(unsigned ms, std::function<void()> f);

}  // namespace bg
