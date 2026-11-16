#include "core/token.hpp"

namespace bg {

Var::Var(TagList t) : m_type(Tags), m_tags(std::make_shared<TagList>(std::move(t))) {}
Var::Var(TokenMap m) : m_type(Map), m_map(std::make_shared<TokenMap>(std::move(m))) {}

Var Var::fromJson(const nlohmann::json& j) {
    switch(j.type()) {
        case nlohmann::json::value_t::null: return Var();
        case nlohmann::json::value_t::boolean: return Var(j.get<bool>());
        case nlohmann::json::value_t::number_integer:
        case nlohmann::json::value_t::number_unsigned: return Var(j.get<long long>());
        case nlohmann::json::value_t::number_float: {
            double d = j.get<double>();
            if(d == (double)(long long)d && std::abs(d) < 9e15) return Var((long long)d);
            return Var(d);
        }
        case nlohmann::json::value_t::string: {
            std::string s = j.get<std::string>();
            if(startsWith(s, "date:")) {
                DateTime d = DateTime::parse(s.substr(5));
                if(d.valid()) return Var(d);
            }
            return Var(s);
        }
        case nlohmann::json::value_t::array: {
            StringList l;
            bool allStrings = true;
            for(auto& e : j) {
                if(!e.is_string()) {
                    allStrings = false;
                    break;
                }
                l.push_back(e.get<std::string>());
            }
            if(allStrings) return Var(l);
            [[fallthrough]];
        }
        default: {
            Var v;
            v.m_type = Json;
            v.m_json = std::make_shared<nlohmann::json>(j);
            return v;
        }
    }
}

bool Var::isEmpty() const {
    switch(m_type) {
        case Null: return true;
        case Int: return m_int == 0;
        case String: return m_str.empty();
        case List: return m_list.empty();
        case Tags: return !m_tags || m_tags->empty();
        case Map: return !m_map || m_map->empty();
        case Json: return !m_json || m_json->empty();
        default: return false;
    }
}

std::string Var::toString() const {
    switch(m_type) {
        case Null: return "";
        case Bool: return m_int ? "true" : "false";
        case Int: return std::to_string(m_int);
        case Double: return fromDouble(m_double);
        case String: return m_str;
        case List: return join(m_list, " ");
        case Date: return m_date.toIso();
        case Tags: {
            StringList l;
            for(auto& t : *m_tags) l.push_back(t.text());
            return join(l, " ");
        }
        case Map: return "";
        case Json: return m_json->is_string() ? m_json->get<std::string>() : m_json->dump();
    }
    return "";
}

long long Var::toInt() const {
    switch(m_type) {
        case Bool:
        case Int: return m_int;
        case Double: return (long long)m_double;
        case String: {
            bool ok = false;
            long long v = bg::toInt(m_str, 0, &ok);
            return ok ? v : (long long)toDouble();
        }
        case Date: return m_date.toUnix();
        case Json: return m_json->is_number() ? m_json->get<long long>() : 0;
        default: return 0;
    }
}

double Var::toDouble() const {
    switch(m_type) {
        case Bool:
        case Int: return (double)m_int;
        case Double: return m_double;
        case String: return bg::toDouble(m_str);
        case Json: return m_json->is_number() ? m_json->get<double>() : 0;
        default: return 0;
    }
}

bool Var::toBool() const {
    switch(m_type) {
        case Bool:
        case Int: return m_int != 0;
        case Double: return m_double != 0;
        case String: return bg::toBool(m_str, !m_str.empty());
        case Json: return m_json->is_boolean() ? m_json->get<bool>() : !m_json->is_null();
        default: return !isEmpty();
    }
}

StringList Var::toList() const {
    switch(m_type) {
        case List: return m_list;
        case Null: return {};
        case Tags: {
            StringList l;
            for(auto& t : *m_tags) l.push_back(t.text());
            return l;
        }
        case String: return m_str.empty() ? StringList{} : StringList{m_str};
        default: return {toString()};
    }
}

const TagList& Var::tags() const {
    static const TagList empty;
    return m_tags ? *m_tags : empty;
}

const TokenMap& Var::map() const {
    static const TokenMap empty;
    return m_map ? *m_map : empty;
}

const nlohmann::json& Var::json() const {
    static const nlohmann::json empty;
    return m_json ? *m_json : empty;
}

nlohmann::json Var::toJson() const {
    switch(m_type) {
        case Null: return nullptr;
        case Bool: return m_int != 0;
        case Int: return m_int;
        case Double: return m_double;
        case String: return m_str;
        case List: return m_list;
        case Date: return "date:" + m_date.toIso();
        case Tags: {
            nlohmann::json a = nlohmann::json::array();
            for(auto& t : *m_tags) a.push_back(t.toJson());
            return a;
        }
        case Map: return nullptr;
        case Json: return *m_json;
    }
    return nullptr;
}

bool Var::operator==(const Var& o) const {
    if(m_type != o.m_type) return toString() == o.toString();
    switch(m_type) {
        case Null: return true;
        case Bool:
        case Int: return m_int == o.m_int;
        case Double: return m_double == o.m_double;
        case String: return m_str == o.m_str;
        case List: return m_list == o.m_list;
        case Date: return m_date == o.m_date;
        default: return toString() == o.toString();
    }
}

Token::Token(Var value, const Var& def) : m_value(std::move(value)) {
    if(!def.isNull() && m_value.isEmpty()) m_value = def;
}

Token::Token(Var value, std::string whatToDo, std::string empty, std::string multiple)
    : m_value(std::move(value)), m_whatToDo(std::move(whatToDo)), m_empty(std::move(empty)), m_multiple(std::move(multiple)) {}

Token::Token(std::function<Var()> lazy) : m_lazy(std::move(lazy)) {}

const Var& Token::value() const {
    if(m_lazy) {
        auto f = std::move(m_lazy);
        m_lazy = nullptr;
        m_value = f();
    }
    return m_value;
}

const Var& tokenValue(const TokenMap& tokens, const std::string& name) {
    static const Var null;
    auto it = tokens.find(name);
    return it == tokens.end() ? null : it->second.value();
}

}  // namespace bg
