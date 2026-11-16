#include "core/filename.hpp"

#include <algorithm>
#include <mutex>

#include "core/js.hpp"
#include "core/logger.hpp"
#include "core/settings.hpp"

namespace bg {

static const char ESCAPE = '^';

// ---- AST plumbing -------------------------------------------------------------

void FilenameNodeRoot::accept(FilenameVisitor& v) const { v.visit(*this); }
void FilenameNodeText::accept(FilenameVisitor& v) const { v.visit(*this); }
void FilenameNodeVariable::accept(FilenameVisitor& v) const { v.visit(*this); }
void FilenameNodeJavaScript::accept(FilenameVisitor& v) const { v.visit(*this); }
void FilenameNodeConditional::accept(FilenameVisitor& v) const { v.visit(*this); }
void FilenameNodeConditionOp::accept(FilenameVisitor& v) const { v.visit(*this); }
void FilenameNodeConditionInvert::accept(FilenameVisitor& v) const { v.visit(*this); }
void FilenameNodeConditionIgnore::accept(FilenameVisitor& v) const { v.visit(*this); }
void FilenameNodeConditionTag::accept(FilenameVisitor& v) const { v.visit(*this); }
void FilenameNodeConditionToken::accept(FilenameVisitor& v) const { v.visit(*this); }
void FilenameNodeConditionJavaScript::accept(FilenameVisitor& v) const { v.visit(*this); }

FilenameNodeConditionTag::FilenameNodeConditionTag(const std::string& text) : tag(text), filter(buildFilter(tag.text())) {}

void FilenameVisitor::visit(const FilenameNodeRoot& n) {
    for(auto& e : n.exprs) e->accept(*this);
}
void FilenameVisitor::visit(const FilenameNodeConditional& n) {
    n.condition->accept(*this);
    if(n.ifTrue) n.ifTrue->accept(*this);
    if(n.ifFalse) n.ifFalse->accept(*this);
}
void FilenameVisitor::visit(const FilenameNodeConditionOp& n) {
    n.left->accept(*this);
    n.right->accept(*this);
}
void FilenameVisitor::visit(const FilenameNodeConditionInvert& n) { n.node->accept(*this); }
void FilenameVisitor::visit(const FilenameNodeConditionIgnore& n) { n.node->accept(*this); }

// Legacy conditionals use the same condition node both in the condition and in the output;
// with unique ownership we keep a copy for each place.
static CondPtr cloneCondition(const FilenameNodeCondition* c) {
    if(auto* t = dynamic_cast<const FilenameNodeConditionTag*>(c)) return std::make_unique<FilenameNodeConditionTag>(t->tag.text());
    if(auto* t = dynamic_cast<const FilenameNodeConditionToken*>(c)) return std::make_unique<FilenameNodeConditionToken>(t->name, t->opts);
    if(auto* t = dynamic_cast<const FilenameNodeConditionInvert*>(c)) return std::make_unique<FilenameNodeConditionInvert>(cloneCondition(t->node.get()));
    if(auto* t = dynamic_cast<const FilenameNodeConditionIgnore*>(c)) return std::make_unique<FilenameNodeConditionIgnore>(cloneCondition(t->node.get()));
    if(auto* t = dynamic_cast<const FilenameNodeConditionJavaScript*>(c)) return std::make_unique<FilenameNodeConditionJavaScript>(t->script);
    if(auto* t = dynamic_cast<const FilenameNodeConditionOp*>(c))
        return std::make_unique<FilenameNodeConditionOp>(t->op, cloneCondition(t->left.get()), cloneCondition(t->right.get()));
    return nullptr;
}

// ---- Parser ----------------------------------------------------------------

struct ParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void FilenameParser::skipSpaces() {
    while(!finished() && g_ascii_isspace(peek())) m_index++;
}

long FilenameParser::indexOf(const std::string& chars, long max) const {
    bool escapeNext = false;
    size_t limit = max < 0 ? m_str.size() : std::min((size_t)max, m_str.size());
    for(size_t i = m_index; i < limit; i++) {
        char c = m_str[i];
        bool isEscape = c == ESCAPE || ((c == '<' || c == '>') && i + 1 < m_str.size() && c == m_str[i + 1]);
        if(isEscape && !escapeNext)
            escapeNext = true;
        else if(chars.find(c) != std::string::npos && !escapeNext)
            return (long)i;
        if(!isEscape && escapeNext) escapeNext = false;
    }
    return -1;
}

std::string FilenameParser::readUntil(const std::string& chars, bool allowEnd) {
    std::string ret;
    bool escapeNext = false;
    while(!finished()) {
        char c = m_str[m_index];
        bool isEscape = c == ESCAPE || ((c == '<' || c == '>') && m_index + 1 < m_str.size() && c == m_str[m_index + 1]);
        if(isEscape && !escapeNext) {
            escapeNext = true;
        } else {
            if(chars.find(c) != std::string::npos && !escapeNext) return ret;
            ret += c;
        }
        if(!isEscape && escapeNext) escapeNext = false;
        m_index++;
    }
    if(!allowEnd) throw ParseError("Unexpected EOF");
    return ret;
}

std::unique_ptr<FilenameNodeRoot> FilenameParser::parseRoot() {
    try {
        auto root = std::make_unique<FilenameNodeRoot>();
        while(!finished()) root->exprs.push_back(parseExpr());
        return root;
    } catch(const std::exception& e) {
        m_error = e.what();
        return nullptr;
    }
}

CondPtr FilenameParser::parseCondition() {
    try {
        return parseConditionNode();
    } catch(const std::exception& e) {
        m_error = e.what();
        return nullptr;
    }
}

NodePtr FilenameParser::parseExprs(const std::string& chars) {
    auto root = std::make_unique<FilenameNodeRoot>();
    while(!finished() && chars.find(peek()) == std::string::npos) root->exprs.push_back(parseExpr(chars));
    if(root->exprs.size() == 1) return std::move(root->exprs[0]);
    return root;
}

NodePtr FilenameParser::parseExpr(const std::string& addChars) {
    if(m_str.compare(m_index, 11, "javascript:") == 0) {
        m_index += 11;
        std::string script = m_str.substr(m_index);
        m_index = m_str.size();
        return std::make_unique<FilenameNodeJavaScript>(script);
    }
    std::string txt = readUntil("<%" + addChars, true);
    if(txt.empty()) {
        char p = peek();
        if(p == '<' && (m_index + 1 >= m_str.size() || m_str[m_index + 1] != '<')) return parseConditional();
        if(p == '%') return parseVariable();
    }
    return std::make_unique<FilenameNodeText>(txt);
}

std::unique_ptr<FilenameNodeVariable> FilenameParser::parseVariable() {
    m_index++;  // %
    std::string name = readUntil(":%");
    Options opts;
    while(peek() != '%') {
        m_index++;  // : or ,
        std::string opt = readUntil("=,%");
        std::string val;
        if(peek() == '=') {
            m_index++;
            val = readUntil(",%");
        }
        opts[opt] = val;
    }
    m_index++;  // %
    return std::make_unique<FilenameNodeVariable>(name, opts);
}

std::unique_ptr<FilenameNodeConditional> FilenameParser::parseConditional() {
    m_index++;  // <
    auto node = std::make_unique<FilenameNodeConditional>();
    long endIndex = indexOf(">");
    long sepIndex = indexOf("?", endIndex);
    if(sepIndex < 0) {
        // Legacy conditional: <text "tag" text %token% -"ignored">
        auto root = std::make_unique<FilenameNodeRoot>();
        std::vector<CondPtr> conds;
        static const std::string stop = ">-!\"%";
        static const std::string stopCond = "\"%";
        while(peek() != '>') {
            bool notStop = stop.find(peek()) == std::string::npos;
            if(notStop) root->exprs.push_back(parseExpr(stop));
            if((peek() == '-' || peek() == '!') && m_index + 1 < m_str.size() &&
               (stopCond.find(m_str[m_index + 1]) == std::string::npos || m_str[m_index + 1] == '>')) {
                NodePtr xpr = parseExpr(">\"%");
                auto* xprTxt = dynamic_cast<FilenameNodeText*>(xpr.get());
                auto* lastTxt = notStop && !root->exprs.empty() ? dynamic_cast<FilenameNodeText*>(root->exprs.back().get()) : nullptr;
                if(xprTxt && lastTxt)
                    lastTxt->text += xprTxt->text;
                else
                    root->exprs.push_back(std::move(xpr));
            }
            if(peek() != '>') {
                CondPtr cond = parseSingleCondition(true);
                conds.push_back(cloneCondition(cond.get()));
                root->exprs.push_back(std::move(cond));
            }
        }
        if(conds.empty()) throw ParseError("No condition found in conditional");
        CondPtr condition = std::move(conds[0]);
        for(size_t i = 1; i < conds.size(); i++)
            condition = std::make_unique<FilenameNodeConditionOp>(FilenameNodeConditionOp::And, std::move(condition), std::move(conds[i]));
        node->condition = std::move(condition);
        if(root->exprs.size() == 1)
            node->ifTrue = std::move(root->exprs[0]);
        else
            node->ifTrue = std::move(root);
    } else {
        node->condition = parseConditionNode();
        if(finished() || peek() != '?') throw ParseError("Expected '?' after condition");
        m_index++;
        node->ifTrue = parseExprs(":>");
        if(peek() == ':') {
            m_index++;
            node->ifFalse = parseExprs(">");
        }
        if(finished() || peek() != '>') throw ParseError("Expected '>' at the end of conditional");
    }
    m_index++;  // >
    return node;
}

CondPtr FilenameParser::parseConditionNode() {
    if(m_str.compare(m_index, 11, "javascript:") == 0) {
        m_index += 11;
        std::string script = m_str.substr(m_index);
        m_index = m_str.size();
        return std::make_unique<FilenameNodeConditionJavaScript>(script);
    }
    skipSpaces();
    std::vector<char> ops;
    std::vector<CondPtr> terms;
    auto reduce = [&] {
        CondPtr right = std::move(terms.back());
        terms.pop_back();
        CondPtr left = std::move(terms.back());
        terms.pop_back();
        char op = ops.back();
        ops.pop_back();
        terms.push_back(std::make_unique<FilenameNodeConditionOp>(op == '&' ? FilenameNodeConditionOp::And : FilenameNodeConditionOp::Or,
                                                                  std::move(left), std::move(right)));
    };
    terms.push_back(parseSingleCondition());
    while(!finished()) {
        skipSpaces();
        char p = peek();
        char c = (p == '"' || p == '%') ? '&' : p;
        if(c != '&' && c != '|') break;
        int prec = c == '&' ? 2 : 1;
        while(!ops.empty()) {
            int stackPrec = ops.back() == '&' ? 2 : 1;
            if(prec > stackPrec) break;
            reduce();
        }
        ops.push_back(c);
        if(p == c) {
            m_index++;
            skipSpaces();
        }
        terms.push_back(parseSingleCondition());
    }
    while(!ops.empty()) reduce();
    skipSpaces();
    if(terms.empty()) return nullptr;
    return std::move(terms.back());
}

CondPtr FilenameParser::parseSingleCondition(bool legacy) {
    char c = peek();
    if(c == '(') {
        m_index++;
        m_parenDepth++;
        CondPtr ret = parseConditionNode();
        if(finished() || peek() != ')') throw ParseError("Expected ')' after condition in parenthesis");
        m_index++;
        m_parenDepth--;
        return ret;
    }
    if(legacy && c == '-') {
        m_index++;
        return std::make_unique<FilenameNodeConditionIgnore>(parseSingleCondition(true));
    }
    if(c == '!' || c == '-') {
        m_index++;
        return std::make_unique<FilenameNodeConditionInvert>(parseSingleCondition());
    }
    if(c == '%') {
        auto var = parseVariable();
        return std::make_unique<FilenameNodeConditionToken>(var->name, var->opts);
    }
    if(c == '"') {
        m_index++;
        std::string tag = readUntil("\"");
        m_index++;
        return std::make_unique<FilenameNodeConditionTag>(tag);
    }
    if(!legacy) {
        std::string tag = m_parenDepth == 0 ? readUntil(" &|?", true) : readUntil(" &|?)", true);
        return std::make_unique<FilenameNodeConditionTag>(tag);
    }
    throw ParseError("Expected '!', '%' or '\"' for condition");
}

// ---- JavaScript variables --------------------------------------------------------

std::string cleanTokenValue(std::string res, bool replaceBlanks) {
    for(const char* c : {"\\", "%", "/", ":", "|", "*", "?", "\"", "<", ">"}) res = replaceAll(res, c, "_");
    for(int i = 0; i < 3; i++) res = replaceAll(res, "__", "_");
    res = trim(res);
    if(!replaceBlanks) res = replaceAll(res, "_", " ");
    return res;
}

static std::string separatorFor(const Settings* settings, const std::string& key) {
    std::string main = settings ? settings->get("Save/separator", " ") : " ";
    std::string sep = settings ? settings->get("Save/" + key + "_sep", main) : main;
    return replaceAll(replaceAll(sep, "\\n", "\n"), "\\r", "\r");
}

void setJavaScriptVariables(js::Context& ctx, js::Value obj, const TokenMap& tokens, const Settings* settings) {
    bool replaceBlanks = settings && settings->getBool("Save/replaceblanks", false);
    for(auto& [name, token] : tokens) {
        const Var& val = token.value();
        if(val.type() == Var::List || val.type() == Var::String) {
            std::string res;
            if(val.type() == Var::List) {
                if(name != "all" && name != "tags") obj.set(name + "s", ctx.fromStringList(val.toList()));
                res = join(val.toList(), separatorFor(settings, name));
            } else {
                res = val.toString();
            }
            if(name != "allo") res = cleanTokenValue(res, replaceBlanks);
            obj.set(name, res);
        } else if(val.type() == Var::Map) {
            js::Value sub = ctx.newObject();
            setJavaScriptVariables(ctx, sub, val.map(), settings);
            obj.set(name, sub);
        } else if(val.type() == Var::Int) {
            obj.set(name, val.toInt());
        } else if(val.type() == Var::Double) {
            obj.set(name, val.toDouble());
        } else if(val.type() == Var::Bool) {
            obj.set(name, val.toBool());
        } else if(val.type() == Var::Date) {
            js::Value d = ctx.eval("new Date(" + std::to_string(val.toDate().toUnixMs()) + ")");
            obj.set(name, d);
        } else if(val.type() == Var::Tags) {
            obj.set(name, ctx.fromStringList(val.toList()));
        } else if(val.type() == Var::Json) {
            obj.set(name, ctx.fromJson(val.json()));
        }
    }
}

// ---- Execution visitor -------------------------------------------------------------

FilenameExecutionVisitor::FilenameExecutionVisitor(const TokenMap& tokens, const Settings* settings)
    : m_tokens(tokens), m_settings(settings) {}

std::string FilenameExecutionVisitor::run(const FilenameNodeRoot& node) {
    m_result.clear();
    node.accept(*this);
    return m_result;
}

void FilenameExecutionVisitor::visit(const FilenameNodeConditional& n) {
    FilenameConditionVisitor cond(m_tokens, m_settings);
    bool valid = cond.run(*n.condition);
    if(valid && n.ifTrue)
        n.ifTrue->accept(*this);
    else if(!valid && n.ifFalse)
        n.ifFalse->accept(*this);
}

void FilenameExecutionVisitor::visit(const FilenameNodeConditionTag& n) { m_result += cleanVariable(n.tag.text()); }
void FilenameExecutionVisitor::visit(const FilenameNodeConditionToken& n) { visitVariable(n.name, n.opts); }

void FilenameExecutionVisitor::visit(const FilenameNodeJavaScript& n) {
    js::Context ctx;
    setJavaScriptVariables(ctx, ctx.global(), m_tokens, m_settings);
    js::Value r = ctx.eval(n.script, "filename");
    if(r.isError()) {
        log("Error in JavaScript filename evaluation: " + r.errorMessage(), LogLevel::Error);
        return;
    }
    m_result += r.toString();
}

static std::string optionsToString(const Options& opts) {
    StringList parts;
    for(auto& [k, v] : opts) parts.push_back(v.empty() ? k : k + "=" + v);
    return join(parts, ",");
}

void FilenameExecutionVisitor::visitVariable(const std::string& fullName, const Options& opts) {
    StringList var = split(fullName, '.');
    std::string name = var.front();
    var.erase(var.begin());
    const TokenMap* context = &m_tokens;
    bool found = true;
    while(found && !var.empty()) {
        auto it = context->find(name);
        if(it != context->end()) {
            const Var& v = it->second.value();
            if(v.type() == Var::Map) {
                context = &v.map();
                name = var.front();
                var.erase(var.begin());
                continue;
            }
            break;
        }
        found = false;
    }
    auto it = context->find(name);
    if(!found || it == context->end()) {
        if(m_keepInvalid || name == "path" || name == "num") {
            std::string o = optionsToString(opts);
            m_result += "%" + fullName + (o.empty() ? "" : ":" + o) + "%";
        }
        return;
    }
    const Var& val = it->second.value();
    std::string res;
    bool clean = false;
    if(val.type() == Var::List || val.type() == Var::Tags) {
        res = listToString(name, val.toList(), opts);
        clean = true;
    } else {
        res = variableToString(name, val, opts);
    }
    auto maxLength = opts.find("maxlength");
    if(maxLength != opts.end()) res = utf8Left(res, (size_t)std::max(0LL, toInt(maxLength->second)));
    if(opts.count("htmlescape")) res = htmlEscape(res);
    bool raw = name == "allo" || startsWith(name, "url_") || name == "filename" || name == "directory" || name == "old_filename" ||
               name == "old_directory";
    if(!raw && !clean) res = cleanVariable(res, opts);
    if(m_escape && opts.count("escape")) res = m_escape(res);
    m_result += res;
}

std::string FilenameExecutionVisitor::variableToString(const std::string& name, const Var& val, const Options& opts) {
    auto lengthIt = opts.find("length");
    switch(val.type()) {
        case Var::Date: {
            DateTime d = val.toDate();
            auto tz = opts.find("timezone");
            if(tz != opts.end() && !tz->second.empty() && tz->second != "server") {
                if(tz->second == "local") d = d.toLocal();
                else {
                    DateTime conv = d.toTimeZone(tz->second);
                    if(!conv.valid()) log("Unknown timezone '" + tz->second + "'", LogLevel::Error);
                    else d = conv;
                }
            }
            auto f = opts.find("format");
            return d.format(f == opts.end() ? "MM-dd-yyyy HH.mm" : f->second);
        }
        case Var::Int:
        case Var::Bool: {
            std::string s = std::to_string(val.toInt());
            if(lengthIt != opts.end()) {
                size_t w = (size_t)std::max(0LL, toInt(lengthIt->second));
                bool neg = !s.empty() && s[0] == '-';
                std::string digits = neg ? s.substr(1) : s;
                if(digits.size() + (neg ? 1 : 0) < w) digits = std::string(w - digits.size() - (neg ? 1 : 0), '0') + digits;
                s = (neg ? "-" : "") + digits;
            }
            return s;
        }
        case Var::Double: {
            auto p = opts.find("precision");
            int precision = p == opts.end() ? 6 : (int)toInt(p->second, 6);
            std::string s = fromDouble(val.toDouble(), precision);
            if(lengthIt != opts.end()) s = leftPad(s, (size_t)std::max(0LL, toInt(lengthIt->second)));
            return s;
        }
        case Var::List:
        case Var::Tags: return listToString(name, val.toList(), opts);
        default: break;
    }
    std::string s = val.toString();
    if(name == "score" && lengthIt != opts.end()) {
        size_t dot = s.find('.');
        size_t length = (size_t)std::max(0LL, toInt(lengthIt->second)) + (dot != std::string::npos ? s.size() - dot : 0);
        s = leftPad(s, length);
    }
    return s;
}

std::string FilenameExecutionVisitor::listToString(const std::string& name, StringList val, const Options& opts) {
    if(opts.count("count")) return variableToString(name, Var((long long)val.size()), opts);
    bool ignoreNs = opts.count("ignorenamespace") > 0, includeNs = opts.count("includenamespace") > 0;
    if(ignoreNs || includeNs) {
        StringList namespaces = tokenValue(m_tokens, "all_namespaces").toList();
        auto nsAt = [&](size_t i) { return name == "all" ? (i < namespaces.size() ? namespaces[i] : std::string()) : name; };
        if(ignoreNs) {
            StringList ignored = split(opts.at("ignorenamespace"), ' ', true);
            StringList filtered, filteredNs;
            for(size_t i = 0; i < val.size(); i++) {
                std::string ns = nsAt(i);
                if(!contains(ignored, ns)) {
                    filtered.push_back(val[i]);
                    filteredNs.push_back(ns);
                }
            }
            val = filtered;
            namespaces = filteredNs;
        }
        if(includeNs) {
            StringList excluded = opts.count("excludenamespace") ? split(opts.at("excludenamespace"), ' ', true) : StringList();
            StringList namespaced;
            for(size_t i = 0; i < val.size(); i++) {
                std::string ns = name == "all" ? (i < namespaces.size() ? namespaces[i] : std::string()) : name;
                namespaced.push_back((!contains(excluded, ns) && !ns.empty() ? ns + ":" : "") + val[i]);
            }
            val = namespaced;
        }
    }
    if(opts.count("sort")) std::sort(val.begin(), val.end());
    auto c = opts.find("case");
    if(c != opts.end() && (c->second == "lower" || c->second == "upper_first" || c->second == "upper" || c->second == "caps")) {
        TagNameFormat fmt(TagNameFormat::caseFromString(c->second), "_");
        for(auto& t : val) t = fmt.formatted(split(t, '_'));
    }
    if(!startsWith(name, "source"))
        for(auto& t : val) t = cleanVariable(t, opts);
    auto sepOpt = opts.find("separator");
    std::string sep = sepOpt != opts.end() ? replaceAll(replaceAll(sepOpt->second, "\\n", "\n"), "\\r", "\r") : separatorFor(m_settings, name);
    return join(val, sep);
}

std::string FilenameExecutionVisitor::cleanVariable(std::string val, const Options& opts) const {
    bool raw = opts.count("raw") > 0;
    if(!opts.count("unsafe") && !raw) {
        for(const char* c : {"\\", "%", "/", ":", "|", "*", "?", "\"", "<", ">"}) val = replaceAll(val, c, "_");
        for(int i = 0; i < 3; i++) val = replaceAll(val, "__", "_");
        val = trim(val);
    }
    bool replaceBlanks = m_settings && m_settings->getBool("Save/replaceblanks", false);
    if(!raw && !opts.count("underscores") && (!replaceBlanks || opts.count("spaces"))) val = replaceAll(val, "_", " ");
    return val;
}

// ---- Condition visitor -------------------------------------------------------------

FilenameConditionVisitor::FilenameConditionVisitor(const TokenMap& tokens, const Settings* settings) : m_tokens(tokens), m_settings(settings) {
    m_tags = tokenValue(tokens, "allos").toList();
}

bool FilenameConditionVisitor::run(const FilenameNodeCondition& node) {
    m_result = true;
    node.accept(*this);
    return m_result;
}

void FilenameConditionVisitor::visit(const FilenameNodeConditionInvert& n) {
    n.node->accept(*this);
    m_result = !m_result;
}

void FilenameConditionVisitor::visit(const FilenameNodeConditionJavaScript& n) {
    js::Context ctx;
    setJavaScriptVariables(ctx, ctx.global(), m_tokens, m_settings);
    js::Value r = ctx.eval(n.script, "condition");
    if(r.isError()) {
        log("Error in JavaScript condition evaluation: " + r.errorMessage(), LogLevel::Error);
        return;
    }
    m_result = r.toBool();
}

void FilenameConditionVisitor::visit(const FilenameNodeConditionOp& n) {
    n.left->accept(*this);
    if((!m_result && n.op == FilenameNodeConditionOp::And) || (m_result && n.op == FilenameNodeConditionOp::Or)) return;
    n.right->accept(*this);
}

void FilenameConditionVisitor::visit(const FilenameNodeConditionTag& n) {
    m_result = contains(m_tags, n.tag.text()) || (n.filter && n.filter->match(m_tokens).empty());
}

void FilenameConditionVisitor::visit(const FilenameNodeConditionToken& n) {
    StringList var = split(n.name, '.');
    const TokenMap* context = &m_tokens;
    for(size_t i = 0; i + 1 < var.size(); i++) {
        auto it = context->find(var[i]);
        if(it == context->end() || it->second.value().type() != Var::Map) {
            m_result = false;
            return;
        }
        context = &it->second.value().map();
    }
    auto it = context->find(var.back());
    m_result = it != context->end() && !it->second.value().isEmpty();
}

// ---- Token resolution ----------------------------------------------------------------

namespace {
struct ResolutionVisitor : FilenameVisitor {
    std::set<std::string> names;
    void visit(const FilenameNodeVariable& n) override { names.insert(n.name); }
    void visit(const FilenameNodeConditionToken& n) override { names.insert(n.name); }
};

std::shared_ptr<ParsedFilename> parseCached(const std::string& format) {
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<ParsedFilename>> cache;
    std::lock_guard<std::mutex> l(mutex);
    auto it = cache.find(format);
    if(it != cache.end()) return it->second;
    auto parsed = std::make_shared<ParsedFilename>();
    FilenameParser parser(format);
    parsed->ast = parser.parseRoot();
    parsed->error = parser.error();
    if(parsed->ast) {
        ResolutionVisitor rv;
        parsed->ast->accept(rv);
        parsed->tokens = rv.names;
    }
    if(cache.size() > 500) cache.clear();
    cache[format] = parsed;
    return parsed;
}
}  // namespace

// ---- Filename ----------------------------------------------------------------

Filename::Filename(std::string format) : m_format(std::move(format)) { m_parsed = parseCached(m_format); }

const std::set<std::string>& Filename::tokens() const { return m_parsed->tokens; }
const std::string& Filename::parseError() const { return m_parsed->error; }

static std::vector<Token> getReplace(const std::string& key, const Token& token, const Settings* settings) {
    std::vector<Token> ret;
    StringList value = token.value().toList();
    if(token.whatToDoDefault().empty() || !settings) {
        ret.emplace_back(Var(value));
        return ret;
    }
    auto s = [&](const std::string& k, const std::string& def) { return settings->get("Save/" + key + k, def); };
    if(s("_sort", "original") == "name") std::sort(value.begin(), value.end());
    if(value.empty()) {
        ret.emplace_back(Var(s("_empty", token.emptyDefault())));
    } else if((long long)value.size() > toInt(s("_multiple_limit", "1"), 1)) {
        std::string whatToDo = s("_multiple", token.whatToDoDefault());
        if(whatToDo == "keepAll") {
            ret.emplace_back(Var(value));
        } else if(whatToDo == "multiple") {
            for(auto& v : value) ret.emplace_back(Var(v));
        } else if(whatToDo == "keepN") {
            long long keepN = std::max(1LL, toInt(s("_multiple_keepN", "1"), 1));
            ret.emplace_back(Var(StringList(value.begin(), value.begin() + std::min((size_t)keepN, value.size()))));
        } else if(whatToDo == "keepNThenAdd") {
            long long keepN = toInt(s("_multiple_keepNThenAdd_keep", "1"), 1);
            std::string thenAdd = s("_multiple_keepNThenAdd_add", " (+ %count%)");
            thenAdd = replaceAll(thenAdd, "%total%", std::to_string(value.size()));
            thenAdd = replaceAll(thenAdd, "%count%", std::to_string((long long)value.size() - keepN));
            StringList kept(value.begin(), value.begin() + std::min((size_t)std::max(1LL, keepN), value.size()));
            if((long long)value.size() > keepN) {
                std::string sep = s("_sep", settings->get("Save/separator", " "));
                ret.emplace_back(Var(join(kept, sep) + thenAdd));
            } else {
                ret.emplace_back(Var(kept));
            }
        } else {
            ret.emplace_back(Var(s("_value", token.multipleDefault())));
        }
    } else {
        ret.emplace_back(Var(value));
    }
    return ret;
}

std::vector<TokenMap> Filename::expandTokens(const TokenMap& tokens, const Settings* settings) const {
    std::vector<TokenMap> ret{tokens};
    bool isJs = startsWith(m_format, "javascript:");
    for(auto& [key, token] : tokens) {
        bool used = isJs ? contains(m_format, key) : m_parsed->tokens.count(key) > 0;
        if(!used) continue;
        if(token.value().type() != Var::List) continue;
        std::vector<Token> reps = getReplace(key, token, settings);
        size_t cnt = ret.size();
        for(size_t i = 0; i < cnt; i++) {
            ret[i][key] = reps[0];
            for(size_t j = 1; j < reps.size(); j++) {
                TokenMap copy = ret[i];
                copy[key] = reps[j];
                ret.push_back(std::move(copy));
            }
        }
    }
    return ret;
}

StringList Filename::path(TokenMap tokens, const Settings* settings, std::string folder, int counter, int flags) const {
    if(!m_parsed->ast) return {};
    tokens["count"] = Token(Var(counter));
    tokens["current_date"] = Token(Var(DateTime::now()));

    if(flags & ConditionalFilenames) {
        Filename override;
        bool hasOverride = false;
        for(auto& fn : getConditionalFilenames(settings)) {
            if(fn.matches(tokens, settings)) {
                if(!fn.path.empty()) folder = fn.path;
                if(!fn.filename.format().empty()) {
                    override = fn.filename;
                    hasOverride = true;
                }
            }
        }
        if(hasOverride) return override.path(tokens, settings, folder, counter, flags & ~ConditionalFilenames);
    }
    StringList fns;
    if(m_format.empty()) return fns;

    for(auto& replaces : expandTokens(tokens, settings)) {
        FilenameExecutionVisitor visitor(replaces, settings);
        visitor.setEscapeMethod(m_escape);
        visitor.setKeepInvalidTokens(flags & KeepInvalidTokens);
        std::string out = visitor.run(*m_parsed->ast);
        if(out.empty()) continue;

        // %num% is resolved against the files already in the destination folder
        static const Regex rxNum("%num(?::[^%]*)?%");
        auto numMatch = rxNum.match(out);
        if(numMatch) {
            std::string hasNum = numMatch->groups[0];
            FilenameParser parser(hasNum);
            std::unique_ptr<FilenameNodeVariable> var;
            try {
                var = parser.parseVariable();
            } catch(...) {
            }
            Options opts = var ? var->opts : Options();
            bool noExt = opts.count("noext") > 0;
            size_t mid = out.rfind('/');
            std::string dir = folder + (mid != std::string::npos ? "/" + out.substr(0, mid) : "");
            std::string right = mid != std::string::npos ? out.substr(mid + 1) : out;
            size_t pos = right.find(hasNum);
            std::string prefix = right.substr(0, pos), suffix = right.substr(pos + hasNum.size());
            std::string ext = tokenValue(replaces, "ext").toString();
            std::string suffixRx = Regex::escape(suffix);
            if(noExt && !ext.empty() && endsWith(suffix, "." + ext))
                suffixRx = Regex::escape(suffix.substr(0, suffix.size() - ext.size() - 1)) + "\\.[^.]+";
            Regex rx("^" + Regex::escape(prefix) + "(\\d+)" + suffixRx + "$");
            long long num = 1;
            for(auto& f : listDir(dir, false, true)) {
                if(endsWith(f, ".tmp")) continue;
                auto m = rx.match(f);
                if(m) num = std::max(num, toInt(m->groups[1]) + 1);
            }
            std::string val = visitor.variableToString("num", Var(num), opts);
            out.replace(numMatch->offsets[0].first, hasNum.size(), val);
        }
        fns.push_back(out);
    }

    for(auto& fn : fns) {
        if(flags & Fix) {
            static const Regex rxTrimDirs(" */ *");
            fn = rxTrimDirs.replace(trim(fn), "/");
            int limit = (flags & CapLength) && settings ? (int)settings->getInt("Save/limit", 0) : 0;
            fn = fixFilename(fn, folder, limit);
        }
        if(flags & IncludeFolder) fn = folder + "/" + fn;
        if(flags & Fix) {
            static const Regex rxMultiDirs("(.)/{2,}");
            fn = rxMultiDirs.replace(fn, "\\1/");
        }
    }
    return fns;
}

bool Filename::isValid(const Settings* settings, std::string* message, const StringList& extraTokens) const {
    auto fail = [&](const std::string& msg) {
        if(message) *message = msg;
        return false;
    };
    if(m_format.empty()) return fail("Filename must not be empty!");
    if(startsWith(m_format, "javascript:")) {
        if(message) *message = "JavaScript expressions can't be validated.";
        return true;
    }
    if(!m_parsed->error.empty()) return fail("Can't compile your filename: " + m_parsed->error);
    const auto& toks = m_parsed->tokens;
    if(!endsWith(m_format, ".%ext%")) return fail("Your filename doesn't end with an extension (%ext%), you may not be able to open saved files.");
    if(!toks.count("md5") && !toks.count("id") && !toks.count("num"))
        return fail("Your filename is not unique to each image, an image may overwrite a previous one. Use %md5% to avoid this.");

    StringList known{"tags", "artist", "contributor", "general", "copyright", "character", "model", "photo_set", "species", "lore",
                     "meta", "filename", "rating", "md5", "website", "websitename", "ext", "all", "id", "search", "search_(\\d+)",
                     "allo", "date", "score", "count", "width", "height", "pool", "url_file", "url_page", "num", "name", "position",
                     "current_date", "author", "authorid", "parentid", "grabber", "grabber_branch", "grabber_version", "filesize",
                     "source", "sources", "url_original", "url_sample", "url_thumbnail", "mpixels", "ratio", "filetype", "md5_forced",
                     "path", "dir", "gallery\\.[^%:]+"};
    for(auto& t : extraTokens) known.push_back(Regex::escape(t));
    if(settings)
        for(auto& [k, _] : getCustoms(settings)) known.push_back(Regex::escape(k));
    static const Regex rx("%(.+?)%");
    for(auto& m : rx.matchAll(m_format)) {
        bool found = false;
        for(auto& k : known) {
            if(Regex("^%(?:gallery\\.)?" + k + "(?::[^%]+)?%$").matches(m.groups[0])) {
                found = true;
                break;
            }
        }
        if(!found) return fail("The %" + m.groups[1] + "% token does not exist and will not be replaced.");
    }
    if(toks.count("num") && settings) {
        long long simultaneous = std::clamp(settings->getInt("Save/simultaneous", 1), 1LL, 10LL);
        if(simultaneous > 1) return fail("The %num% token does not play well with simultaneous downloads. Download images one at a time instead.");
    }
    if(!toks.count("md5") && !toks.count("website") && !toks.count("websitename") && !toks.count("count") && toks.count("id")) {
        if(message) *message = "%id% is only unique for a given site, the same ID can identify different images on different sites.";
        return true;
    }
    if(message) *message = "Valid filename!";
    return true;
}

bool Filename::needTemporaryFile(const TokenMap& tokens) const {
    if(startsWith(m_format, "javascript:")) return false;
    const auto& toks = m_parsed->tokens;
    auto missing = [&](const char* name, bool numeric) {
        if(!toks.count(name)) return false;
        auto it = tokens.find(name);
        if(it == tokens.end()) return true;
        return numeric ? it->second.value().toInt() <= 0 : it->second.value().toString().empty();
    };
    return missing("md5", false) || missing("filesize", true) || missing("width", true) || missing("height", true);
}

int Filename::needExactTags(const StringList& forcedTokens, const StringList& customTokens) const {
    if(startsWith(m_format, "javascript:")) return 2;
    if(contains(forcedTokens, "*")) return 2;
    const auto& toks = m_parsed->tokens;
    if(toks.count("filename") && contains(forcedTokens, "filename")) return 2;
    if(toks.count("date") && contains(forcedTokens, "date")) return 2;
    if(contains(forcedTokens, "tags")) {
        for(const char* t : {"tags", "all", "allo", "artist", "contributor", "copyright", "character", "model", "photo_set", "species", "lore", "meta", "general"})
            if(toks.count(t)) return 2;
        for(auto& t : customTokens)
            if(toks.count(t)) return 2;
    }
    for(const char* t : {"artist", "contributor", "copyright", "character", "model", "photo_set", "species", "lore", "meta", "general"})
        if(toks.count(t)) return 1;
    if(contains(m_format, "includenamespace")) return 1;
    return 0;
}

// ---- Conditional filenames & helpers -----------------------------------------------

ConditionalFilename::ConditionalFilename(std::string cond, std::string fn, std::string p)
    : condition(std::move(cond)), filename(std::move(fn)), path(std::move(p)) {
    if(condition.empty()) return;
    FilenameParser parser(condition);
    CondPtr ast = parser.parseCondition();
    if(!parser.error().empty()) {
        log("Error parsing condition '" + condition + "': " + parser.error(), LogLevel::Error);
        return;
    }
    m_ast = std::shared_ptr<FilenameNodeCondition>(std::move(ast));
}

bool ConditionalFilename::matches(const TokenMap& tokens, const Settings* settings) const {
    if(!m_ast) return false;
    FilenameConditionVisitor v(tokens, settings);
    return v.run(*m_ast);
}

std::map<std::string, StringList> getCustoms(const Settings* settings) {
    std::map<std::string, StringList> out;
    if(!settings) return out;
    for(auto& key : settings->childKeys("Save/Customs")) out[key] = splitAny(settings->get("Save/Customs/" + key), " \n", true);
    return out;
}

std::vector<ConditionalFilename> getConditionalFilenames(const Settings* settings) {
    std::vector<ConditionalFilename> out;
    if(!settings) return out;
    size_t count = settings->childKeys("Filenames").size() / 3;
    for(size_t i = 0; i < count; i++) {
        std::string k = "Filenames/" + std::to_string(i);
        if(settings->contains(k + "_cond")) out.emplace_back(settings->get(k + "_cond"), settings->get(k + "_fn"), settings->get(k + "_dir"));
    }
    return out;
}

// Linux rules: each path component at most 255 bytes, the file name capped to maxLength bytes.
std::string fixFilename(std::string fn, std::string path, int maxLength) {
    if(!path.empty() && !endsWith(path, "/") && !fn.empty()) path += "/";
    maxLength = maxLength <= 0 ? 255 : maxLength;
    std::string filename = path + fn;
    StringList parts = split(filename, '/');
    std::string file, ext;
    if(!fn.empty()) {
        file = parts.back();
        parts.pop_back();
        size_t dot = file.rfind('.');
        if(dot != std::string::npos) {
            ext = file.substr(dot + 1);
            file = file.substr(0, dot);
        }
    }
    for(auto& part : parts) part = utf8TruncateBytes(part, 255);
    size_t extLen = ext.empty() ? 0 : ext.size() + 1;
    file = utf8TruncateBytes(file, (size_t)std::max(1, maxLength - (int)extLen));
    std::string dirPart = join(parts, "/");
    filename = (dirPart.empty() ? "" : dirPart + (fn.empty() ? "" : "/")) + file;
    // Strip the folder part back off
    if(!fn.empty()) {
        size_t pathGroups = std::count(path.begin(), path.end(), '/');
        size_t index = std::string::npos;
        long idx = -1;
        for(size_t i = 0; i < pathGroups; i++) {
            index = filename.find('/', (size_t)(idx + 1));
            if(index == std::string::npos) break;
            idx = (long)index;
        }
        filename = filename + (ext.empty() ? "" : "." + ext);
        filename = filename.substr((size_t)(idx + 1));
    } else {
        filename += ext.empty() ? "" : "." + ext;
    }
    return filename;
}

std::string executeVariableExpression(const std::string& variable, const TokenMap& tokens) {
    FilenameParser parser(variable);
    std::unique_ptr<FilenameNodeVariable> var;
    try {
        var = parser.parseVariable();
    } catch(...) {
        return "";
    }
    FilenameNodeRoot root;
    root.exprs.push_back(std::move(var));
    FilenameExecutionVisitor v(tokens, nullptr);
    return v.run(root);
}

}  // namespace bg
