#include "core/tags.hpp"

#include <sqlite3.h>

#include <algorithm>

#include "core/logger.hpp"

namespace bg {

// ---- TagType ---------------------------------------------------------------

TagType::TagType(const std::string& name) {
    m_unknown = name.empty() || name == "unknown";
    m_name = name.empty() ? "unknown" : name;
    if(!m_unknown) {
        size_t sp = m_name.find(' ');
        if(sp != std::string::npos) m_name = Tag::getType(m_name.substr(0, sp));
    }
}

int TagType::number() const {
    if(m_unknown) return -1;
    static const std::map<std::string, int> types{{"general", 0}, {"artist", 1},    {"copyright", 3},
                                                  {"character", 4}, {"model", 5}, {"photo_set", 6}};
    auto it = types.find(m_name);
    return it == types.end() ? -1 : it->second;
}

// ---- Tag -------------------------------------------------------------------

Tag::Tag(const std::string& text, const TagType& type, int count, StringList related)
    : Tag(0, text, type, count, std::move(related)) {}

Tag::Tag(int id, const std::string& text, const TagType& type, int count, StringList related)
    : m_id(id), m_type(type), m_count(count), m_related(std::move(related)) {
    m_text = replaceAll(decodeHtmlEntities(text), " ", "_");
    if(m_type.isUnknown() || m_type.name() == "origin") {
        // Some artist names end with " (artist)"
        if(endsWith(m_text, "(artist)")) m_type = TagType("artist");
        size_t sep = m_text.find(':');
        if(sep != std::string::npos) {
            static const std::map<int, std::string> prep{{0, "artist"}, {1, "copyright"}, {2, "character"}, {3, "model"},
                                                         {4, "species"}, {5, "meta"},     {6, "unknown"},   {7, "oc"}};
            std::string pre = getType(m_text.substr(0, sep));
            for(auto& [idx, name] : prep) {
                if(name == pre) {
                    m_type = TagType(getType(name, prep));
                    m_text = m_text.substr(sep + 1);
                    break;
                }
            }
        }
    }
}

std::string Tag::getType(std::string type, const std::map<int, std::string>& ids) {
    type = trim(toLower(type));
    size_t comma = type.find(", ");
    if(comma != std::string::npos) type = trim(type.substr(0, comma));
    static const std::map<std::string, std::string> aliases{
        {"idol", "model"},     {"series", "copyright"},  {"mangaka", "artist"},          {"origin", "artist"},
        {"game", "copyright"}, {"studio", "circle"},     {"source", "general"},          {"genre", "general"},
        {"oc", "character"},   {"medium", "meta"},       {"metadata", "meta"},           {"character group", "general"}};
    auto it = aliases.find(type);
    if(it != aliases.end()) return it->second;
    if(type.size() == 1) {
        bool ok = false;
        int id = (int)toInt(type, 0, &ok);
        auto jt = ids.find(id);
        if(ok && jt != ids.end()) return jt->second;
    }
    return type;
}

nlohmann::json Tag::toJson() const {
    nlohmann::json j;
    j["text"] = m_text;
    if(m_id > 0) j["id"] = m_id;
    if(!m_type.isUnknown()) j["type"] = m_type.name();
    if(m_count > 0) j["count"] = m_count;
    if(!m_related.empty()) j["related"] = m_related;
    return j;
}

Tag Tag::fromJson(const nlohmann::json& j) {
    Tag t;
    if(j.is_string()) return Tag(j.get<std::string>());
    if(!j.is_object()) return t;
    t.m_text = j.value("text", "");
    t.m_id = j.value("id", 0);
    if(j.contains("type") && j["type"].is_string()) t.m_type = TagType(j["type"].get<std::string>());
    t.m_count = j.value("count", 0);
    if(j.contains("related") && j["related"].is_array())
        for(auto& r : j["related"])
            if(r.is_string()) t.m_related.push_back(r.get<std::string>());
    return t;
}

void sortTags(TagList& tags, const std::string& order) {
    auto byName = [](const Tag& a, const Tag& b) { return naturalCompare(a.text(), b.text()) < 0; };
    if(order == "name") {
        std::stable_sort(tags.begin(), tags.end(), byName);
    } else if(order == "count") {
        std::stable_sort(tags.begin(), tags.end(), [](const Tag& a, const Tag& b) { return a.count() > b.count(); });
    } else if(order == "type") {
        static const StringList typeOrder{"unknown", "model", "meta", "lore", "species", "artist", "contributor", "character", "copyright"};
        auto rank = [](const Tag& t) {
            auto it = std::find(typeOrder.begin(), typeOrder.end(), t.type().name());
            return it == typeOrder.end() ? -1 : (int)(it - typeOrder.begin());
        };
        std::stable_sort(tags.begin(), tags.end(), [&](const Tag& a, const Tag& b) {
            int ra = rank(a), rb = rank(b);
            return ra == rb ? byName(a, b) : ra > rb;
        });
    }
}

// ---- TagNameFormat ---------------------------------------------------------

TagNameFormat::CaseFormat TagNameFormat::caseFromString(const std::string& s) {
    if(s == "upper_first") return UpperFirst;
    if(s == "upper") return Upper;
    if(s == "caps") return Caps;
    return Lower;
}

std::string TagNameFormat::formatted(const StringList& words) const {
    std::string r;
    for(size_t i = 0; i < words.size(); i++) {
        if(i) r += m_sep;
        r += formatted(words[i], (int)i);
    }
    return r;
}

std::string TagNameFormat::formatted(const std::string& word, int index) const {
    switch(m_case) {
        case Lower: return toLower(word);
        case Upper:
        case UpperFirst: {
            std::string r = toLower(word);
            return (index == 0 || m_case == Upper) ? upperFirst(r) : r;
        }
        case Caps: return toUpper(word);
    }
    return word;
}

// ---- TagTypeDatabase -------------------------------------------------------

TagTypeDatabase::TagTypeDatabase(std::string readPath, std::string writePath)
    : m_readPath(std::move(readPath)), m_writePath(std::move(writePath)) {}

bool TagTypeDatabase::load() {
    if(m_loaded) return true;
    std::string path = fileExists(m_writePath) ? m_writePath : m_readPath;
    auto data = readFile(path);
    if(!data) return false;
    for(auto& line : split(*data, '\n', true)) {
        StringList parts = split(trim(line), ',');
        if(parts.size() != 2) continue;
        int id = (int)toInt(parts[0]);
        m_types[id] = TagType(parts[1]);
        m_inverted[parts[1]] = id;
        m_maxId = std::max(m_maxId, id);
    }
    m_loaded = true;
    return true;
}

bool TagTypeDatabase::save() {
    if(!m_pending) return true;
    std::string out;
    for(auto& [id, t] : m_types) out += std::to_string(id) + "," + t.name() + "\n";
    m_pending = false;
    return writeFile(m_writePath, out);
}

void TagTypeDatabase::setAll(const std::vector<TagTypeWithId>& types) {
    m_types.clear();
    m_inverted.clear();
    for(auto& t : types) {
        m_types[t.id] = TagType(t.name);
        m_inverted[t.name] = t.id;
        m_maxId = std::max(m_maxId, t.id);
    }
    m_loaded = true;
    m_pending = true;
}

int TagTypeDatabase::get(const TagType& type, bool create) {
    auto it = m_inverted.find(type.name());
    if(it != m_inverted.end()) return it->second;
    if(!create) return -1;
    int id = ++m_maxId;
    m_types[id] = type;
    m_inverted[type.name()] = id;
    m_pending = true;
    return id;
}

// ---- TagDatabase -----------------------------------------------------------

TagDatabase::TagDatabase(std::string typesRead, std::string typesWrite, std::string dbPath)
    : m_types(std::move(typesRead), std::move(typesWrite)), m_path(std::move(dbPath)) {
    m_textMode = endsWith(m_path, ".txt");
}

TagDatabase::~TagDatabase() { close(); }

bool TagDatabase::open() {
    if(m_textMode) {
        m_memory.clear();
        for(auto& line : readLines(m_path)) {
            StringList parts = split(line, ',');
            if(parts.size() < 2) continue;
            m_memory[parts[0]] = {parts.size() > 2 ? (int)toInt(parts[2]) : 0, (int)toInt(parts[1], -1)};
        }
        return true;
    }
    if(m_db) return true;
    if(!fileExists(m_path)) return false;  // only created when tags are actually stored
    if(sqlite3_open_v2(m_path.c_str(), &m_db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        log("Could not open tag database " + m_path + ": " + sqlite3_errmsg(m_db), LogLevel::Warning);
        sqlite3_close(m_db);
        m_db = nullptr;
        return false;
    }
    return true;
}

void TagDatabase::close() {
    if(m_db) sqlite3_close(m_db);
    m_db = nullptr;
}

void TagDatabase::setTags(const TagList& tags, bool createTagTypes) {
    if(m_textMode) {
        std::string out;
        for(auto& t : tags) {
            int type = m_types.get(t.type(), createTagTypes);
            m_memory[t.text()] = {t.id(), type};
        }
        for(auto& [name, v] : m_memory) out += name + "," + std::to_string(v.second) + "," + std::to_string(v.first) + "\n";
        writeFile(m_path, out);
        m_types.save();
        return;
    }
    if(!m_db) {
        ensureParent(m_path);
        if(sqlite3_open(m_path.c_str(), &m_db) != SQLITE_OK) {
            log("Could not create tag database " + m_path, LogLevel::Error);
            close();
            return;
        }
    }
    sqlite3_exec(m_db, "CREATE TABLE IF NOT EXISTS tags (id INT, tag VARCHAR(255), ttype INT);"
                       "CREATE INDEX IF NOT EXISTS tags_tag ON tags(tag);",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(m_db, "BEGIN TRANSACTION", nullptr, nullptr, nullptr);
    sqlite3_stmt* del = nullptr;
    sqlite3_stmt* ins = nullptr;
    sqlite3_prepare_v2(m_db, "DELETE FROM tags WHERE tag = ?", -1, &del, nullptr);
    sqlite3_prepare_v2(m_db, "INSERT INTO tags (id, tag, ttype) VALUES (?, ?, ?)", -1, &ins, nullptr);
    for(auto& t : tags) {
        int type = m_types.get(t.type(), createTagTypes);
        if(type < 0) continue;
        sqlite3_bind_text(del, 1, t.text().c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(del);
        sqlite3_reset(del);
        sqlite3_bind_int(ins, 1, t.id());
        sqlite3_bind_text(ins, 2, t.text().c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(ins, 3, type);
        sqlite3_step(ins);
        sqlite3_reset(ins);
    }
    sqlite3_finalize(del);
    sqlite3_finalize(ins);
    sqlite3_exec(m_db, "COMMIT", nullptr, nullptr, nullptr);
    m_types.save();
}

std::map<std::string, TagType> TagDatabase::getTagTypes(const StringList& tags) {
    std::map<std::string, TagType> out;
    if(tags.empty()) return out;
    const auto& types = m_types.all();
    if(m_textMode) {
        for(auto& t : tags) {
            auto it = m_memory.find(t);
            if(it == m_memory.end()) continue;
            auto ty = types.find(it->second.second);
            if(ty != types.end()) out[t] = ty->second;
        }
        return out;
    }
    if(!m_db && !open()) return out;
    sqlite3_stmt* st = nullptr;
    if(sqlite3_prepare_v2(m_db, "SELECT ttype FROM tags WHERE tag = ?", -1, &st, nullptr) != SQLITE_OK) return out;
    for(auto& t : tags) {
        sqlite3_bind_text(st, 1, t.c_str(), -1, SQLITE_TRANSIENT);
        if(sqlite3_step(st) == SQLITE_ROW) {
            auto ty = types.find(sqlite3_column_int(st, 0));
            if(ty != types.end()) out[t] = ty->second;
        }
        sqlite3_reset(st);
    }
    sqlite3_finalize(st);
    return out;
}

std::map<std::string, int> TagDatabase::getTagIds(const StringList& tags) {
    std::map<std::string, int> out;
    if(tags.empty()) return out;
    if(m_textMode) {
        for(auto& t : tags) {
            auto it = m_memory.find(t);
            if(it != m_memory.end() && it->second.first > 0) out[t] = it->second.first;
        }
        return out;
    }
    if(!m_db && !open()) return out;
    sqlite3_stmt* st = nullptr;
    if(sqlite3_prepare_v2(m_db, "SELECT id FROM tags WHERE tag = ?", -1, &st, nullptr) != SQLITE_OK) return out;
    for(auto& t : tags) {
        sqlite3_bind_text(st, 1, t.c_str(), -1, SQLITE_TRANSIENT);
        if(sqlite3_step(st) == SQLITE_ROW) out[t] = sqlite3_column_int(st, 0);
        sqlite3_reset(st);
    }
    sqlite3_finalize(st);
    return out;
}

int TagDatabase::count() {
    if(m_textMode) return (int)m_memory.size();
    if(!m_db && !open()) return 0;
    sqlite3_stmt* st = nullptr;
    int n = 0;
    if(sqlite3_prepare_v2(m_db, "SELECT COUNT(*) FROM tags", -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

}  // namespace bg
