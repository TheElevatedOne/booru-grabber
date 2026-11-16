#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/util.hpp"

struct sqlite3;

namespace bg {

class TagType {
public:
    TagType() = default;
    explicit TagType(const std::string& name);
    bool isUnknown() const { return m_unknown; }
    const std::string& name() const { return m_name; }
    int number() const;  // generic id (general=0, artist=1, copyright=3, character=4, model=5, photo_set=6)
    bool operator==(const TagType& o) const { return (m_unknown && o.m_unknown) || m_name == o.m_name; }
    bool operator!=(const TagType& o) const { return !(*this == o); }

private:
    bool m_unknown = true;
    std::string m_name = "unknown";
};

class Tag {
public:
    Tag() = default;
    explicit Tag(const std::string& text, const TagType& type = TagType(), int count = 0, StringList related = {});
    Tag(int id, const std::string& text, const TagType& type, int count = 0, StringList related = {});
    static std::string getType(std::string type, const std::map<int, std::string>& ids = {});

    int id() const { return m_id; }
    const std::string& text() const { return m_text; }
    const TagType& type() const { return m_type; }
    int count() const { return m_count; }
    const StringList& related() const { return m_related; }
    void setType(const TagType& t) { m_type = t; }
    void setCount(int c) { m_count = c; }
    void setText(const std::string& t) { m_text = t; }

    nlohmann::json toJson() const;
    static Tag fromJson(const nlohmann::json& j);
    bool operator==(const Tag& o) const { return m_text == o.m_text; }

private:
    int m_id = 0;
    std::string m_text;
    TagType m_type;
    int m_count = 0;
    StringList m_related;
};

using TagList = std::vector<Tag>;

void sortTags(TagList& tags, const std::string& order);  // "type", "name" or "count"

class TagNameFormat {
public:
    enum CaseFormat { Lower, UpperFirst, Upper, Caps };
    TagNameFormat() = default;
    TagNameFormat(CaseFormat c, std::string sep) : m_case(c), m_sep(std::move(sep)) {}
    static CaseFormat caseFromString(const std::string& s);
    std::string formatted(const StringList& words) const;
    std::string formatted(const std::string& word, int index) const;
    CaseFormat caseFormat() const { return m_case; }
    const std::string& wordSeparator() const { return m_sep; }

private:
    CaseFormat m_case = Lower;
    std::string m_sep = "_";
};

struct TagTypeWithId {
    int id;
    std::string name;
};

// "tag-types.txt": lines of "id,name"
class TagTypeDatabase {
public:
    TagTypeDatabase(std::string readPath, std::string writePath);
    bool load();
    bool save();
    const std::map<int, TagType>& all() const { return m_types; }
    void setAll(const std::vector<TagTypeWithId>& types);
    int get(const TagType& type, bool create = false);

private:
    std::string m_readPath, m_writePath;
    std::map<int, TagType> m_types;
    std::map<std::string, int> m_inverted;
    int m_maxId = 0;
    bool m_pending = false, m_loaded = false;
};

// Per-site tag database: either tags.db (sqlite) or a legacy tags.txt ("name,type" lines)
class TagDatabase {
public:
    TagDatabase(std::string typesReadPath, std::string typesWritePath, std::string dbPath);
    ~TagDatabase();
    bool open();
    void close();
    bool loadTypes() { return m_types.load(); }
    bool save() { return m_types.save(); }
    const std::map<int, TagType>& tagTypes() const { return m_types.all(); }
    void setTagTypes(const std::vector<TagTypeWithId>& types) { m_types.setAll(types); }
    int tagTypeNumber(const TagType& t) { return m_types.get(t); }
    void setTags(const TagList& tags, bool createTagTypes = false);
    std::map<std::string, TagType> getTagTypes(const StringList& tags);
    std::map<std::string, int> getTagIds(const StringList& tags);
    int count();

private:
    TagTypeDatabase m_types;
    std::string m_path;
    bool m_textMode = false;
    sqlite3* m_db = nullptr;
    std::map<std::string, std::pair<int, int>> m_memory;  // text mode: name -> (id, type id)
};

}  // namespace bg
