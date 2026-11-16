#include "core/filters.hpp"

#include <cmath>

#include "core/filename.hpp"
#include "core/logger.hpp"
#include "core/settings.hpp"

namespace bg {

// ---- TagFilter -------------------------------------------------------------

TagFilter::TagFilter(std::string tag, bool invert) : Filter(invert), m_tag(std::move(tag)) {
    if(m_tag.find('*') != std::string::npos) m_regex = std::make_unique<Regex>(Regex::fromWildcard(m_tag, true));
}

std::string TagFilter::toString(bool escape) const { return std::string(m_invert ? "-" : "") + (escape ? replaceAll(m_tag, ":", "::") : m_tag); }

std::string TagFilter::match(const TokenMap& tokens, bool invert) const {
    if(m_invert) invert = !invert;
    StringList tags = tokenValue(tokens, "allos").toList();
    bool cond = false;
    for(auto& t : tags) {
        if(m_regex ? m_regex->matches(t) : t == m_tag) {
            cond = true;
            break;
        }
    }
    if(!cond && !invert) return "image does not contain \"" + m_tag + "\"";
    if(cond && invert) return "image contains \"" + m_tag + "\"";
    return "";
}

// ---- TokenFilter -----------------------------------------------------------

std::string TokenFilter::toString(bool) const { return std::string(m_invert ? "-" : "") + "%" + m_token + "%"; }

std::string TokenFilter::match(const TokenMap& tokens, bool invert) const {
    if(m_invert) invert = !invert;
    bool cond = tokens.count(m_token) && !tokenValue(tokens, m_token).isEmpty();
    if(cond && invert) return "image has a \"" + m_token + "\" token";
    if(!cond && !invert) return "image does not have a \"" + m_token + "\" token";
    return "";
}

// ---- MetaFilter ------------------------------------------------------------

std::string MetaFilter::toString(bool) const { return std::string(m_invert ? "-" : "") + m_type + ":" + m_val; }

template <typename T, typename F>
static bool rangeCheck(F conv, const T& input, const std::string& val) {
    if(startsWith(val, "..") || startsWith(val, "<=")) return input <= conv(val.substr(2));
    if(endsWith(val, "..")) return input >= conv(val.substr(0, val.size() - 2));
    if(startsWith(val, ">=")) return input >= conv(val.substr(2));
    if(startsWith(val, "<")) return input < conv(val.substr(1));
    if(startsWith(val, ">")) return input > conv(val.substr(1));
    size_t dots = val.find("..");
    if(dots != std::string::npos) return input >= conv(val.substr(0, dots)) && input <= conv(val.substr(dots + 2));
    return input == conv(val);
}

static DateTime ageToDate(const std::string& text, const DateTime& now) {
    static const Regex rx("^(\\d+)(\\w+)$");
    auto m = rx.match(text);
    if(!m) return {};
    int count = (int)toInt(m->groups[1]);
    const std::string& type = m->groups[2];
    if(startsWith(type, "y")) return now.addYears(-count);
    if(startsWith(type, "mo")) return now.addMonths(-count);
    if(startsWith(type, "w")) return now.addDays(-count * 7);
    if(startsWith(type, "d")) return now.addDays(-count);
    if(startsWith(type, "h")) return now.addSecs(-(int64_t)count * 3600);
    if(startsWith(type, "mi")) return now.addSecs(-(int64_t)count * 60);
    if(startsWith(type, "s")) return now.addSecs(-count);
    return {};
}

std::string MetaFilter::match(const TokenMap& tokens, bool invert) const {
    if(m_invert) invert = !invert;

    if(m_type == "grabber") {
        bool cond = containsCi(tokenValue(tokens, "grabber").toList(), m_val);
        if(!cond && !invert) return "image is not \"" + m_val + "\"";
        if(cond && invert) return "image is \"" + m_val + "\"";
        return "";
    }

    if(m_type == "age") {
        if(!tokens.count("date")) return "an image needs a date to be filtered by age";
        DateTime date = tokenValue(tokens, "date").toDate();
        DateTime now = tokens.count("TESTS_now") ? tokenValue(tokens, "TESTS_now").toDate() : DateTime::nowUtc();
        bool cond = rangeCheck([&](const std::string& s) { return ageToDate(s, now); }, date, m_val);
        if(cond && !invert) return "image's age does not match";
        if(!cond && invert) return "image's age matches";
        return "";
    }

    // "%var%:value" syntax
    if(startsWith(m_type, "%") && endsWith(m_type, "%")) {
        std::string input = executeVariableExpression(m_type, tokens);
        bool isNumber = false;
        long long n = toInt(input, 0, &isNumber);
        bool cond = isNumber ? rangeCheck([](const std::string& s) { return toInt(s); }, n, m_val) : input == m_val;
        if(!cond && !invert) return "expression '" + m_type + "' ('" + input + "') does not match";
        if(cond && invert) return "expression '" + m_type + "' ('" + input + "') matches";
        return "";
    }

    if(!tokens.count(m_type)) {
        StringList keys;
        for(auto& [k, _] : tokens) keys.push_back(k);
        return "unknown type \"" + m_type + "\" (available types: \"" + join(keys, "\", \"") + "\")";
    }

    const Var& token = tokenValue(tokens, m_type);
    if(token.type() == Var::Int || token.type() == Var::Date || m_type == "score") {
        bool cond;
        if(token.type() == Var::Date)
            cond = rangeCheck([](const std::string& s) { return DateTime::parseDate(s); }, token.toDate(), m_val);
        else if(m_type == "score")
            cond = rangeCheck([](const std::string& s) { return std::llround(toDouble(s) * 1000); }, std::llround(token.toDouble() * 1000), m_val);
        else if(m_type == "filesize")
            cond = rangeCheck([](const std::string& s) { return parseFileSize(s); }, token.toInt(), m_val);
        else
            cond = rangeCheck([](const std::string& s) { return toInt(s); }, token.toInt(), m_val);
        if(!cond && !invert) return "image's " + m_type + " does not match";
        if(cond && invert) return "image's " + m_type + " matches";
        return "";
    }

    if(m_type == "rating") {
        static const std::map<std::string, std::string> assoc{{"g", "general"}, {"s", "safe"}, {"q", "questionable"}, {"e", "explicit"}};
        auto it = assoc.find(m_val);
        std::string val = it != assoc.end() ? it->second : m_val;
        std::string rating = toLower(token.toString());
        bool cond = !val.empty() && !rating.empty() && rating[0] == val[0];
        if(!cond && !invert) return "image is not \"" + val + "\"";
        if(cond && invert) return "image is \"" + val + "\"";
        return "";
    }
    if(m_type == "source") {
        bool cond = startsWith(toLower(token.toString()), toLower(m_val));
        if(!cond && !invert) return "image's source does not start with \"" + m_val + "\"";
        if(cond && invert) return "image's source starts with \"" + m_val + "\"";
        return "";
    }
    bool cond = token.toString() == m_val;
    if(!cond && !invert) return "image's " + m_type + " does not match";
    if(cond && invert) return "image's " + m_type + " matches";
    return "";
}

// ---- factory -----------------------------------------------------------------

std::unique_ptr<Filter> buildFilter(std::string filter) {
    bool invert = false;
    if(startsWith(filter, "-")) {
        filter = filter.substr(1);
        invert = true;
    }
    if(filter.size() >= 2 && startsWith(filter, "%") && endsWith(filter, "%") && filter.find('%', 1) == filter.size() - 1)
        return std::make_unique<TokenFilter>(filter.substr(1, filter.size() - 2), invert);
    size_t colon = startsWith(filter, "%") ? filter.rfind(':') : filter.find(':');
    if(colon != std::string::npos) {
        if(colon > 0 && colon < filter.size() - 1 && filter[colon + 1] != ':')
            return std::make_unique<MetaFilter>(toLower(filter.substr(0, colon)), toLower(filter.substr(colon + 1)), invert);
        filter = replaceAll(filter, "::", ":");
    }
    filter = trim(filter);
    if(!filter.empty()) return std::make_unique<TagFilter>(filter, invert);
    return nullptr;
}

// ---- Blacklist -------------------------------------------------------------

Blacklist::Blacklist(const StringList& lines) {
    for(auto& l : lines) add(l);
}

int Blacklist::indexOf(const StringList& tags) const {
    for(size_t i = 0; i < m_filters.size(); i++) {
        const auto& filters = m_filters[i];
        if(filters.size() != tags.size()) continue;
        bool all = true;
        for(size_t j = 0; j < tags.size(); j++)
            if(!iequals(filters[j]->toString(false), tags[j])) {
                all = false;
                break;
            }
        if(all) return (int)i;
    }
    return -1;
}

bool Blacklist::contains(const std::string& tag) const { return indexOf({tag}) >= 0; }

void Blacklist::add(const std::string& line) {
    std::string l = trim(line);
    if(l.empty() || l[0] == '#') return;
    add(split(l, ' ', true));
}

void Blacklist::add(const StringList& tags) {
    std::vector<std::shared_ptr<Filter>> filters;
    for(auto& t : tags) {
        auto f = buildFilter(t);
        if(f) filters.push_back(std::move(f));
    }
    if(!filters.empty()) m_filters.push_back(std::move(filters));
}

bool Blacklist::remove(const std::string& line) {
    int idx = indexOf(split(trim(line), ' ', true));
    if(idx < 0) return false;
    m_filters.erase(m_filters.begin() + idx);
    return true;
}

StringList Blacklist::lines() const {
    StringList out;
    for(auto& filters : m_filters) {
        StringList parts;
        for(auto& f : filters) parts.push_back(f->toString());
        out.push_back(join(parts, " "));
    }
    return out;
}

std::string Blacklist::toString() const { return join(lines(), "\n"); }

StringList Blacklist::match(const TokenMap& tokens, bool invert) const {
    StringList detected;
    for(auto& filters : m_filters) {
        bool all = true;
        StringList res;
        for(auto& f : filters) {
            if(f->match(tokens, invert).empty()) {
                all = false;
                break;
            }
            res.push_back(f->toString(false));
        }
        if(all) detected.push_back(join(res, " "));
    }
    return detected;
}

// ---- PostFilter ------------------------------------------------------------

PostFilter::PostFilter(const std::string& filters, const Settings* settings) : m_settings(settings) {
    if(trim(filters).empty()) return;
    static const Regex advanced("[()&|]");
    if(advanced.matches(filters)) {
        FilenameParser parser(filters);
        CondPtr ast = parser.parseCondition();
        if(!parser.error().empty()) {
            log("Error parsing post-filter '" + filters + "': " + parser.error(), LogLevel::Error);
            return;
        }
        m_ast = std::shared_ptr<FilenameNodeCondition>(std::move(ast));
        return;
    }
    for(auto& f : split(filters, ' ', true)) {
        auto filter = buildFilter(f);
        if(filter) m_filters.push_back(std::move(filter));
    }
}

PostFilter::PostFilter(const StringList& filters, const Settings* settings) : PostFilter(join(filters, " "), settings) {}

StringList PostFilter::match(const TokenMap& tokens) const {
    StringList ret;
    if(m_ast) {
        FilenameConditionVisitor v(tokens, m_settings);
        if(!v.run(*m_ast)) ret.push_back("post-filter");
    }
    for(auto& f : m_filters) {
        std::string err = f->match(tokens);
        if(!err.empty()) ret.push_back(err);
    }
    return ret;
}

// ---- TagFilterList ---------------------------------------------------------

void TagFilterList::add(const std::string& word) {
    if(word.empty()) return;
    if(word.find('*') != std::string::npos)
        m_star.push_back(Regex::fromWildcard(word, true));
    else
        m_raw.insert(word);
}

void TagFilterList::add(const StringList& words) {
    for(auto& w : words) add(w);
}

void TagFilterList::clear() {
    m_raw.clear();
    m_star.clear();
}

TagList TagFilterList::filterTags(const TagList& tags) const {
    TagList out;
    for(auto& t : tags) {
        if(m_raw.count(t.text())) continue;
        bool removed = false;
        for(auto& r : m_star)
            if(r.matches(t.text())) {
                removed = true;
                break;
            }
        if(!removed) out.push_back(t);
    }
    return out;
}

}  // namespace bg
