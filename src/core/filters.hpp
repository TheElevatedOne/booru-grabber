// Image filters: blacklist, post-filters and the tag conditions of filenames.
// match() returns an empty string when the image passes, a reason otherwise.
#pragma once

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/token.hpp"

namespace bg {

class Settings;
struct FilenameNodeCondition;

class Filter {
public:
    explicit Filter(bool invert) : m_invert(invert) {}
    virtual ~Filter() = default;
    virtual std::string toString(bool escape = true) const = 0;
    virtual std::string match(const TokenMap& tokens, bool invert = false) const = 0;
    bool invert() const { return m_invert; }

protected:
    bool m_invert;
};

class TagFilter : public Filter {
public:
    TagFilter(std::string tag, bool invert);
    std::string toString(bool escape = true) const override;
    std::string match(const TokenMap& tokens, bool invert = false) const override;

private:
    std::string m_tag;
    std::unique_ptr<Regex> m_regex;
};

class TokenFilter : public Filter {
public:
    TokenFilter(std::string token, bool invert) : Filter(invert), m_token(std::move(token)) {}
    std::string toString(bool escape = true) const override;
    std::string match(const TokenMap& tokens, bool invert = false) const override;

private:
    std::string m_token;
};

class MetaFilter : public Filter {
public:
    MetaFilter(std::string type, std::string val, bool invert) : Filter(invert), m_type(std::move(type)), m_val(std::move(val)) {}
    std::string toString(bool escape = true) const override;
    std::string match(const TokenMap& tokens, bool invert = false) const override;

private:
    std::string m_type, m_val;
};

std::unique_ptr<Filter> buildFilter(std::string filter);

class Blacklist {
public:
    Blacklist() = default;
    explicit Blacklist(const StringList& lines);
    bool empty() const { return m_filters.empty(); }
    bool contains(const std::string& tag) const;
    void clear() { m_filters.clear(); }
    void add(const std::string& line);  // one line = filters that must all match
    void add(const StringList& tags);
    bool remove(const std::string& line);
    std::string toString() const;  // one rule per line
    StringList lines() const;
    StringList match(const TokenMap& tokens, bool invert = true) const;

private:
    int indexOf(const StringList& tags) const;
    std::vector<std::vector<std::shared_ptr<Filter>>> m_filters;
};

class PostFilter {
public:
    PostFilter() = default;
    explicit PostFilter(const std::string& filters, const Settings* settings = nullptr);
    explicit PostFilter(const StringList& filters, const Settings* settings = nullptr);
    StringList match(const TokenMap& tokens) const;
    bool empty() const { return m_filters.empty() && !m_ast; }

private:
    std::vector<std::shared_ptr<Filter>> m_filters;
    std::shared_ptr<FilenameNodeCondition> m_ast;
    const Settings* m_settings = nullptr;
};

// Removed tags (exact names or wildcards)
class TagFilterList {
public:
    void add(const std::string& word);
    void add(const StringList& words);
    void clear();
    TagList filterTags(const TagList& tags) const;
    bool contains(const std::string& word) const { return m_raw.count(word) > 0; }

private:
    std::set<std::string> m_raw;
    std::vector<Regex> m_star;
};

}  // namespace bg
