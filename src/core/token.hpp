// Dynamically typed token values used by filenames, filters and conditions.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "core/datetime.hpp"
#include "core/tags.hpp"

namespace bg {

class Token;
using TokenMap = std::map<std::string, Token>;

class Var {
public:
    enum Type { Null, Bool, Int, Double, String, List, Date, Tags, Map, Json };

    Var() = default;
    Var(bool b) : m_type(Bool), m_int(b) {}
    Var(int i) : m_type(Int), m_int(i) {}
    Var(long i) : m_type(Int), m_int(i) {}
    Var(long long i) : m_type(Int), m_int(i) {}
    Var(unsigned long long i) : m_type(Int), m_int((long long)i) {}
    Var(double d) : m_type(Double), m_double(d) {}
    Var(const char* s) : m_type(String), m_str(s) {}
    Var(std::string s) : m_type(String), m_str(std::move(s)) {}
    Var(StringList l) : m_type(List), m_list(std::move(l)) {}
    Var(DateTime d) : m_type(Date), m_date(std::move(d)) {}
    Var(TagList t);
    Var(TokenMap m);
    static Var fromJson(const nlohmann::json& j);  // keeps JSON objects/arrays of non-strings as Json

    Type type() const { return m_type; }
    bool isNull() const { return m_type == Null; }
    bool isEmpty() const;  // Grabber's isVariantEmpty
    std::string toString() const;
    long long toInt() const;
    double toDouble() const;
    bool toBool() const;
    StringList toList() const;
    const DateTime& toDate() const { return m_date; }
    const TagList& tags() const;
    const TokenMap& map() const;
    const nlohmann::json& json() const;
    nlohmann::json toJson() const;
    bool operator==(const Var& o) const;

private:
    Type m_type = Null;
    long long m_int = 0;
    double m_double = 0;
    std::string m_str;
    StringList m_list;
    DateTime m_date;
    std::shared_ptr<TagList> m_tags;
    std::shared_ptr<TokenMap> m_map;
    std::shared_ptr<nlohmann::json> m_json;
};

class Token {
public:
    Token() = default;
    Token(Var value, const Var& def = Var());
    Token(Var value, std::string whatToDoDefault, std::string emptyDefault, std::string multipleDefault);
    explicit Token(std::function<Var()> lazy);

    const Var& value() const;
    std::string toString() const { return value().toString(); }
    const std::string& whatToDoDefault() const { return m_whatToDo; }
    const std::string& emptyDefault() const { return m_empty; }
    const std::string& multipleDefault() const { return m_multiple; }

private:
    mutable Var m_value;
    mutable std::function<Var()> m_lazy;
    std::string m_whatToDo, m_empty, m_multiple;
};

// Returns the token value or a Null var
const Var& tokenValue(const TokenMap& tokens, const std::string& name);

}  // namespace bg
