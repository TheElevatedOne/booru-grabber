// JavaScript engine (quickjs-ng) used by the site sources and "javascript:" filenames.
//
// All JS runs on the main thread. One shared context holds helper.js and every source's model;
// filename expressions get a short-lived context of their own.
#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

#include "core/util.hpp"

struct JSContext;
struct JSRuntime;

namespace bg::js {

// Opaque handle matching quickjs' JSValue layout without exposing quickjs.h
class Value {
public:
    Value();
    ~Value();
    Value(const Value& o);
    Value(Value&& o) noexcept;
    Value& operator=(const Value& o);
    Value& operator=(Value&& o) noexcept;

    bool isUndefined() const;
    bool isNull() const;
    bool isNullish() const { return isUndefined() || isNull(); }
    bool isString() const;
    bool isNumber() const;
    bool isBool() const;
    bool isObject() const;
    bool isArray() const;
    bool isFunction() const;
    bool isError() const { return m_error; }  // thrown exception or Error object returned by a call
    bool isIntegral() const;

    std::string toString() const;
    long long toInt() const;
    double toNumber() const;
    bool toBool() const;
    StringList toStringList() const;  // arrays, or a single string
    StringMap toStringMap() const;
    nlohmann::json toJson() const;
    std::string errorMessage() const;  // "Uncaught exception at line N: message"

    Value prop(const std::string& name) const;
    Value at(uint32_t index) const;
    uint32_t length() const;
    bool has(const std::string& name) const;  // property exists and is not undefined
    StringList keys() const;                  // own enumerable string keys
    void set(const std::string& name, const Value& v);
    void set(const std::string& name, const std::string& v);
    void set(const std::string& name, const char* v) { set(name, std::string(v)); }
    void set(const std::string& name, double v);
    void set(const std::string& name, long long v);
    void set(const std::string& name, int v) { set(name, (long long)v); }
    void set(const std::string& name, bool v);
    void setAt(uint32_t index, const Value& v);
    Value call(const std::vector<Value>& args = {}) const;
    Value callMethod(const std::string& name, const std::vector<Value>& args = {}) const;
    // Walks "a.b.c"; returns undefined as soon as a link is missing
    Value path(const std::string& dotted) const;

    JSContext* context() const { return m_ctx; }

    // internal
    struct Raw {
        unsigned char bytes[16];
    };
    Value(JSContext* ctx, const Raw& raw, bool error = false);  // takes ownership
    const Raw& raw() const { return m_raw; }

private:
    JSContext* m_ctx = nullptr;
    Raw m_raw;
    bool m_error = false;
};

class Context {
public:
    // helperFile: optional helper.js evaluated after the native Grabber/console objects are set up
    explicit Context(const std::string& helperFile = "");
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    Value eval(const std::string& code, const std::string& filename = "<eval>");
    Value global() const;
    Value undefined() const;
    Value null() const;
    Value newObject() const;
    Value newArray() const;
    Value newString(const std::string& s) const;
    Value newNumber(double d) const;
    Value newInt(long long i) const;
    Value newBool(bool b) const;
    Value fromStringList(const StringList& l) const;
    Value fromStringMap(const StringMap& m) const;
    Value fromJson(const nlohmann::json& j) const;
    JSContext* raw() const { return m_ctx; }

private:
    JSContext* m_ctx = nullptr;
};

// Shared context for site sources (helper.js loaded from the sites directory)
Context& sourceContext();
void setHelperFile(const std::string& path);

// Values of <input name="..."> elements of an HTML page (used for CSRF tokens)
StringMap htmlInputValues(const std::string& html, const StringList& names);

// Evaluate a standalone expression with the given global variables (filenames, conditions)
Value evalWithVariables(Context& ctx, const std::string& code);

}  // namespace bg::js
