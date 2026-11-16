// Filename formats: "%artist%/%copyright%/%md5%.%ext%", conditionals "<%artist%?...:...>",
// legacy conditionals "<"tag"text>", and "javascript:" expressions.
#pragma once

#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/filters.hpp"
#include "core/token.hpp"

namespace bg {

class Settings;

// ---- AST -------------------------------------------------------------------

struct FilenameVisitor;

struct FilenameNode {
    virtual ~FilenameNode() = default;
    virtual void accept(FilenameVisitor& v) const = 0;
};
using NodePtr = std::unique_ptr<FilenameNode>;
using Options = std::map<std::string, std::string>;

struct FilenameNodeCondition : FilenameNode {};
using CondPtr = std::unique_ptr<FilenameNodeCondition>;

struct FilenameNodeRoot : FilenameNode {
    std::vector<NodePtr> exprs;
    void accept(FilenameVisitor& v) const override;
};
struct FilenameNodeText : FilenameNode {
    std::string text;
    explicit FilenameNodeText(std::string t) : text(std::move(t)) {}
    void accept(FilenameVisitor& v) const override;
};
struct FilenameNodeVariable : FilenameNode {
    std::string name;
    Options opts;
    FilenameNodeVariable(std::string n, Options o) : name(std::move(n)), opts(std::move(o)) {}
    void accept(FilenameVisitor& v) const override;
};
struct FilenameNodeJavaScript : FilenameNode {
    std::string script;
    explicit FilenameNodeJavaScript(std::string s) : script(std::move(s)) {}
    void accept(FilenameVisitor& v) const override;
};
struct FilenameNodeConditional : FilenameNode {
    CondPtr condition;
    NodePtr ifTrue, ifFalse;
    void accept(FilenameVisitor& v) const override;
};
struct FilenameNodeConditionOp : FilenameNodeCondition {
    enum Op { And, Or } op;
    CondPtr left, right;
    FilenameNodeConditionOp(Op o, CondPtr l, CondPtr r) : op(o), left(std::move(l)), right(std::move(r)) {}
    void accept(FilenameVisitor& v) const override;
};
struct FilenameNodeConditionInvert : FilenameNodeCondition {
    CondPtr node;
    explicit FilenameNodeConditionInvert(CondPtr n) : node(std::move(n)) {}
    void accept(FilenameVisitor& v) const override;
};
struct FilenameNodeConditionIgnore : FilenameNodeCondition {
    CondPtr node;
    explicit FilenameNodeConditionIgnore(CondPtr n) : node(std::move(n)) {}
    void accept(FilenameVisitor& v) const override;
};
struct FilenameNodeConditionTag : FilenameNodeCondition {
    Tag tag;
    std::unique_ptr<Filter> filter;
    explicit FilenameNodeConditionTag(const std::string& text);
    void accept(FilenameVisitor& v) const override;
};
struct FilenameNodeConditionToken : FilenameNodeCondition {
    std::string name;
    Options opts;
    FilenameNodeConditionToken(std::string n, Options o) : name(std::move(n)), opts(std::move(o)) {}
    void accept(FilenameVisitor& v) const override;
};
struct FilenameNodeConditionJavaScript : FilenameNodeCondition {
    std::string script;
    explicit FilenameNodeConditionJavaScript(std::string s) : script(std::move(s)) {}
    void accept(FilenameVisitor& v) const override;
};

struct FilenameVisitor {
    virtual ~FilenameVisitor() = default;
    virtual void visit(const FilenameNodeRoot& n);
    virtual void visit(const FilenameNodeText&) {}
    virtual void visit(const FilenameNodeVariable&) {}
    virtual void visit(const FilenameNodeJavaScript&) {}
    virtual void visit(const FilenameNodeConditional& n);
    virtual void visit(const FilenameNodeConditionOp& n);
    virtual void visit(const FilenameNodeConditionInvert& n);
    virtual void visit(const FilenameNodeConditionIgnore& n);
    virtual void visit(const FilenameNodeConditionTag&) {}
    virtual void visit(const FilenameNodeConditionToken&) {}
    virtual void visit(const FilenameNodeConditionJavaScript&) {}
};

class FilenameParser {
public:
    explicit FilenameParser(std::string str) : m_str(std::move(str)) {}
    std::unique_ptr<FilenameNodeRoot> parseRoot();
    CondPtr parseCondition();
    std::unique_ptr<FilenameNodeVariable> parseVariable();  // at "%"
    const std::string& error() const { return m_error; }

private:
    char peek() const { return m_index < m_str.size() ? m_str[m_index] : '\0'; }
    bool finished() const { return m_index >= m_str.size(); }
    void skipSpaces();
    long indexOf(const std::string& chars, long max = -1) const;
    std::string readUntil(const std::string& chars, bool allowEnd = false);
    NodePtr parseExprs(const std::string& chars);
    NodePtr parseExpr(const std::string& addChars = "");
    std::unique_ptr<FilenameNodeConditional> parseConditional();
    CondPtr parseConditionNode();
    CondPtr parseSingleCondition(bool legacy = false);

    std::string m_str;
    size_t m_index = 0;
    int m_parenDepth = 0;
    std::string m_error;
};

// Executes a filename AST into a string with the given tokens
class FilenameExecutionVisitor : public FilenameVisitor {
public:
    FilenameExecutionVisitor(const TokenMap& tokens, const Settings* settings);
    std::string run(const FilenameNodeRoot& node);
    void setKeepInvalidTokens(bool keep) { m_keepInvalid = keep; }
    void setEscapeMethod(std::string (*fn)(const std::string&)) { m_escape = fn; }
    std::string variableToString(const std::string& name, const Var& value, const Options& opts);

    void visit(const FilenameNodeConditional& n) override;
    void visit(const FilenameNodeConditionIgnore&) override {}
    void visit(const FilenameNodeConditionTag& n) override;
    void visit(const FilenameNodeConditionToken& n) override;
    void visit(const FilenameNodeJavaScript& n) override;
    void visit(const FilenameNodeText& n) override { m_result += n.text; }
    void visit(const FilenameNodeVariable& n) override { visitVariable(n.name, n.opts); }

private:
    void visitVariable(const std::string& fullName, const Options& opts);
    std::string listToString(const std::string& name, StringList val, const Options& opts);
    std::string cleanVariable(std::string val, const Options& opts = {}) const;

    const TokenMap& m_tokens;
    const Settings* m_settings;
    std::string m_result;
    bool m_keepInvalid = false;
    std::string (*m_escape)(const std::string&) = nullptr;
};

class FilenameConditionVisitor : public FilenameVisitor {
public:
    FilenameConditionVisitor(const TokenMap& tokens, const Settings* settings);
    bool run(const FilenameNodeCondition& node);
    void visit(const FilenameNodeConditionInvert& n) override;
    void visit(const FilenameNodeConditionJavaScript& n) override;
    void visit(const FilenameNodeConditionOp& n) override;
    void visit(const FilenameNodeConditionTag& n) override;
    void visit(const FilenameNodeConditionToken& n) override;

private:
    const TokenMap& m_tokens;
    const Settings* m_settings;
    StringList m_tags;
    bool m_result = true;
};

// Sets token values as JS globals (shared by filename and condition JS)
class Settings;
namespace js {
class Context;
class Value;
}  // namespace js
void setJavaScriptVariables(js::Context& ctx, js::Value obj, const TokenMap& tokens, const Settings* settings);
std::string cleanTokenValue(std::string val, bool replaceBlanks);

// ---- Filename --------------------------------------------------------------

struct ParsedFilename {
    std::unique_ptr<FilenameNodeRoot> ast;
    std::set<std::string> tokens;
    std::string error;
};

class ConditionalFilename;

class Filename {
public:
    enum Flag {
        None = 0,
        ConditionalFilenames = 1,
        ExpandConditionals = 2,
        CapLength = 4,
        Fix = 8,
        IncludeFolder = 16,
        KeepInvalidTokens = 32,
        Complex = ConditionalFilenames | ExpandConditionals,
        File = CapLength | Fix,
        Path = File | IncludeFolder,
        Default = Complex | File,
        Full = Complex | Path,
    };

    Filename() : Filename("") {}
    explicit Filename(std::string format);
    const std::string& format() const { return m_format; }
    bool empty() const { return m_format.empty(); }
    void setEscapeMethod(std::string (*fn)(const std::string&)) { m_escape = fn; }

    StringList path(TokenMap tokens, const Settings* settings, std::string folder = "", int counter = 0, int flags = Default) const;
    // Returns true when usable; message describes the first problem (or "Valid filename!")
    bool isValid(const Settings* settings, std::string* message = nullptr, const StringList& extraTokens = {}) const;
    bool needTemporaryFile(const TokenMap& tokens) const;
    int needExactTags(const StringList& forcedTokens = {}, const StringList& customTokens = {}) const;
    std::vector<TokenMap> expandTokens(const TokenMap& tokens, const Settings* settings) const;
    const std::set<std::string>& tokens() const;
    const std::string& parseError() const;

private:
    std::string m_format;
    std::shared_ptr<ParsedFilename> m_parsed;
    std::string (*m_escape)(const std::string&) = nullptr;
};

class ConditionalFilename {
public:
    ConditionalFilename(std::string condition, std::string filename, std::string path);
    bool matches(const TokenMap& tokens, const Settings* settings) const;
    std::string condition;
    Filename filename;
    std::string path;

private:
    std::shared_ptr<FilenameNodeCondition> m_ast;
};

std::map<std::string, StringList> getCustoms(const Settings* settings);
std::vector<ConditionalFilename> getConditionalFilenames(const Settings* settings);
std::string fixFilename(std::string filename, std::string path = "", int maxLength = 0);
std::string executeVariableExpression(const std::string& variable, const TokenMap& tokens);

}  // namespace bg
