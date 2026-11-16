#include "core/js.hpp"

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <quickjs.h>

#include <cstring>
#include <memory>
#include <unordered_set>

#include "core/logger.hpp"
#include "lexbor/css/css.h"
#include "lexbor/html/html.h"
#include "lexbor/selectors/selectors.h"

namespace bg::js {

static_assert(sizeof(JSValue) == sizeof(Value::Raw), "JSValue size mismatch");

static inline JSValue J(const Value::Raw& r) {
    JSValue v;
    memcpy(&v, &r, sizeof v);
    return v;
}
static inline Value::Raw R(JSValue v) {
    Value::Raw r;
    memcpy(&r, &v, sizeof v);
    return r;
}
static inline Value wrap(JSContext* ctx, JSValue v, bool error = false) { return Value(ctx, R(v), error); }

// ---- runtime ---------------------------------------------------------------

namespace {
JSRuntime* g_rt = nullptr;
int64_t g_deadline = 0;  // ms; 0 = no limit
std::string g_helperFile;
JSClassID g_htmlClass = 0;

int interruptHandler(JSContext*, void*) { return g_deadline > 0 && nowMs() > g_deadline ? 1 : 0; }

struct Deadline {
    int64_t previous;
    explicit Deadline(int64_t ms) : previous(g_deadline) {
        if(g_deadline == 0) g_deadline = nowMs() + ms;
    }
    ~Deadline() { g_deadline = previous; }
};

JSRuntime* runtime() {
    if(!g_rt) {
        g_rt = JS_NewRuntime();
        JS_SetMaxStackSize(g_rt, 4 * 1024 * 1024);
        JS_SetInterruptHandler(g_rt, interruptHandler, nullptr);
    }
    return g_rt;
}

std::string toStdString(JSContext* ctx, JSValueConst v) {
    size_t len = 0;
    const char* s = JS_ToCStringLen2(ctx, &len, v, false);
    if(!s) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return "";
    }
    std::string r(s, len);
    JS_FreeCString(ctx, s);
    return r;
}

std::string argString(JSContext* ctx, int argc, JSValueConst* argv, int i) {
    if(i >= argc || JS_IsUndefined(argv[i]) || JS_IsNull(argv[i])) return "";
    return toStdString(ctx, argv[i]);
}
}  // namespace

// ---- Value -----------------------------------------------------------------

Value::Value() : m_raw(R(JS_UNDEFINED)) {}
Value::Value(JSContext* ctx, const Raw& raw, bool error) : m_ctx(ctx), m_raw(raw), m_error(error) {}
Value::~Value() {
    if(m_ctx) JS_FreeValue(m_ctx, J(m_raw));
}
Value::Value(const Value& o) : m_ctx(o.m_ctx), m_raw(o.m_ctx ? R(JS_DupValue(o.m_ctx, J(o.m_raw))) : o.m_raw), m_error(o.m_error) {}
Value::Value(Value&& o) noexcept : m_ctx(o.m_ctx), m_raw(o.m_raw), m_error(o.m_error) {
    o.m_ctx = nullptr;
    o.m_raw = R(JS_UNDEFINED);
}
Value& Value::operator=(const Value& o) {
    if(this == &o) return *this;
    Value tmp(o);
    *this = std::move(tmp);
    return *this;
}
Value& Value::operator=(Value&& o) noexcept {
    if(this == &o) return *this;
    if(m_ctx) JS_FreeValue(m_ctx, J(m_raw));
    m_ctx = o.m_ctx;
    m_raw = o.m_raw;
    m_error = o.m_error;
    o.m_ctx = nullptr;
    o.m_raw = R(JS_UNDEFINED);
    return *this;
}

bool Value::isUndefined() const { return JS_IsUndefined(J(m_raw)); }
bool Value::isNull() const { return JS_IsNull(J(m_raw)); }
bool Value::isString() const { return JS_IsString(J(m_raw)); }
bool Value::isNumber() const { return JS_IsNumber(J(m_raw)); }
bool Value::isBool() const { return JS_IsBool(J(m_raw)); }
bool Value::isObject() const { return JS_IsObject(J(m_raw)); }
bool Value::isArray() const { return m_ctx && JS_IsArray(J(m_raw)); }
bool Value::isFunction() const { return m_ctx && JS_IsFunction(m_ctx, J(m_raw)); }
bool Value::isIntegral() const {
    if(!isNumber()) return false;
    double d = toNumber();
    return d == (double)(long long)d;
}

std::string Value::toString() const {
    if(!m_ctx || isUndefined()) return isUndefined() ? "undefined" : "";
    return toStdString(m_ctx, J(m_raw));
}

long long Value::toInt() const {
    if(!m_ctx) return 0;
    if(isString()) return bg::toInt(toString());
    int64_t r = 0;
    if(JS_ToInt64(m_ctx, &r, J(m_raw)) < 0) {
        JS_FreeValue(m_ctx, JS_GetException(m_ctx));
        return 0;
    }
    return r;
}

double Value::toNumber() const {
    if(!m_ctx) return 0;
    double r = 0;
    if(JS_ToFloat64(m_ctx, &r, J(m_raw)) < 0) {
        JS_FreeValue(m_ctx, JS_GetException(m_ctx));
        return 0;
    }
    return r;
}

bool Value::toBool() const {
    if(!m_ctx) return false;
    int r = JS_ToBool(m_ctx, J(m_raw));
    return r > 0;
}

StringList Value::toStringList() const {
    StringList out;
    if(isArray()) {
        uint32_t n = length();
        for(uint32_t i = 0; i < n; i++) out.push_back(at(i).toString());
    } else if(!isNullish()) {
        out.push_back(toString());
    }
    return out;
}

StringMap Value::toStringMap() const {
    StringMap out;
    for(auto& k : keys()) out[k] = prop(k).toString();
    return out;
}

nlohmann::json Value::toJson() const {
    if(!m_ctx || isNullish()) return nullptr;
    if(isBool()) return toBool();
    if(isNumber()) {
        double d = toNumber();
        if(d == (double)(long long)d && std::abs(d) < 9e15) return (long long)d;
        return d;
    }
    if(isString()) return toString();
    if(isArray()) {
        nlohmann::json a = nlohmann::json::array();
        uint32_t n = length();
        for(uint32_t i = 0; i < n; i++) a.push_back(at(i).toJson());
        return a;
    }
    if(isObject() && !isFunction()) {
        nlohmann::json o = nlohmann::json::object();
        for(auto& k : keys()) {
            Value v = prop(k);
            if(!v.isUndefined() && !v.isFunction()) o[k] = v.toJson();
        }
        return o;
    }
    return toString();
}

std::string Value::errorMessage() const {
    if(!m_ctx) return "";
    std::string msg = toString();
    std::string line;
    if(isObject()) {
        Value ln = prop("lineNumber");
        if(ln.isNumber()) line = std::to_string(ln.toInt());
        if(line.empty()) {
            // quickjs stacks look like "    at parse (model.js:123:45)"
            std::string stack = prop("stack").toString();
            static const Regex rx(":(\\d+)(?::\\d+)?\\)?\\s*$", false, false, true);
            for(auto& l : split(stack, '\n', true)) {
                auto m = rx.match(l);
                if(m) {
                    line = m->groups[1];
                    break;
                }
            }
        }
    }
    return "Uncaught exception at line " + (line.empty() ? std::string("?") : line) + ": " + msg;
}

Value Value::prop(const std::string& name) const {
    if(!m_ctx || !isObject()) return Value();
    return wrap(m_ctx, JS_GetPropertyStr(m_ctx, J(m_raw), name.c_str()));
}

Value Value::at(uint32_t i) const {
    if(!m_ctx || !isObject()) return Value();
    return wrap(m_ctx, JS_GetPropertyUint32(m_ctx, J(m_raw), i));
}

uint32_t Value::length() const {
    if(!isObject()) return 0;
    long long n = prop("length").toInt();
    return n < 0 ? 0 : (uint32_t)n;
}

bool Value::has(const std::string& name) const {
    if(!isObject()) return false;
    return !prop(name).isUndefined();
}

StringList Value::keys() const {
    StringList out;
    if(!m_ctx || !isObject()) return out;
    JSPropertyEnum* tab = nullptr;
    uint32_t len = 0;
    if(JS_GetOwnPropertyNames(m_ctx, &tab, &len, J(m_raw), JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
        JS_FreeValue(m_ctx, JS_GetException(m_ctx));
        return out;
    }
    for(uint32_t i = 0; i < len; i++) {
        const char* s = JS_AtomToCString(m_ctx, tab[i].atom);
        if(s) {
            out.emplace_back(s);
            JS_FreeCString(m_ctx, s);
        }
    }
    JS_FreePropertyEnum(m_ctx, tab, len);
    return out;
}

void Value::set(const std::string& name, const Value& v) {
    if(!m_ctx || !isObject()) return;
    JS_SetPropertyStr(m_ctx, J(m_raw), name.c_str(), JS_DupValue(m_ctx, J(v.m_raw)));
}
void Value::set(const std::string& name, const std::string& v) {
    if(!m_ctx || !isObject()) return;
    JS_SetPropertyStr(m_ctx, J(m_raw), name.c_str(), JS_NewStringLen(m_ctx, v.data(), v.size()));
}
void Value::set(const std::string& name, double v) {
    if(!m_ctx || !isObject()) return;
    JS_SetPropertyStr(m_ctx, J(m_raw), name.c_str(), JS_NewFloat64(m_ctx, v));
}
void Value::set(const std::string& name, long long v) {
    if(!m_ctx || !isObject()) return;
    JS_SetPropertyStr(m_ctx, J(m_raw), name.c_str(), JS_NewInt64(m_ctx, v));
}
void Value::set(const std::string& name, bool v) {
    if(!m_ctx || !isObject()) return;
    JS_SetPropertyStr(m_ctx, J(m_raw), name.c_str(), JS_NewBool(m_ctx, v));
}
void Value::setAt(uint32_t i, const Value& v) {
    if(!m_ctx || !isObject()) return;
    JS_SetPropertyUint32(m_ctx, J(m_raw), i, JS_DupValue(m_ctx, J(v.m_raw)));
}

static Value callImpl(JSContext* ctx, JSValueConst fn, JSValueConst thisObj, const std::vector<Value>& args) {
    if(!ctx || !JS_IsFunction(ctx, fn)) return Value();
    std::vector<JSValue> argv;
    argv.reserve(args.size());
    for(auto& a : args) argv.push_back(J(a.raw()));
    Deadline d(15000);
    JSValue r = JS_Call(ctx, fn, thisObj, (int)argv.size(), argv.data());
    if(JS_IsException(r)) return wrap(ctx, JS_GetException(ctx), true);
    bool isErr = JS_IsError(r);
    return wrap(ctx, r, isErr);
}

Value Value::call(const std::vector<Value>& args) const { return callImpl(m_ctx, J(m_raw), JS_UNDEFINED, args); }

Value Value::callMethod(const std::string& name, const std::vector<Value>& args) const {
    Value fn = prop(name);
    return callImpl(m_ctx, J(fn.m_raw), J(m_raw), args);
}

Value Value::path(const std::string& dotted) const {
    Value cur = *this;
    for(auto& part : split(dotted, '.')) {
        if(cur.isNullish()) return Value();
        cur = cur.prop(part);
    }
    return cur;
}

// ---- HTML (lexbor) -----------------------------------------------------------

namespace {
struct HtmlDoc {
    lxb_html_document_t* doc = nullptr;
    ~HtmlDoc() {
        if(doc) lxb_html_document_destroy(doc);
    }
};
struct HtmlRef {
    std::shared_ptr<HtmlDoc> doc;
    lxb_dom_node_t* node;
};

void htmlFinalizer(JSRuntime*, JSValue val) { delete static_cast<HtmlRef*>(JS_GetOpaque(val, g_htmlClass)); }

JSValue newHtmlNode(JSContext* ctx, std::shared_ptr<HtmlDoc> doc, lxb_dom_node_t* node);

HtmlRef* thisNode(JSContext* ctx, JSValueConst thisVal) {
    auto* ref = static_cast<HtmlRef*>(JS_GetOpaque(thisVal, g_htmlClass));
    if(!ref) JS_ThrowTypeError(ctx, "not an HTML node");
    return ref;
}

std::string lexborToString(lexbor_str_t& str, lxb_dom_node_t* node) {
    std::string r = str.data ? std::string((const char*)str.data, str.length) : std::string();
    if(str.data) lexbor_str_destroy(&str, node->owner_document->text, false);
    return r;
}

JSValue jsOuterHTML(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    auto* ref = thisNode(ctx, thisVal);
    if(!ref) return JS_EXCEPTION;
    lexbor_str_t str = {nullptr, 0};
    if(lxb_html_serialize_tree_str(ref->node, &str) != LXB_STATUS_OK) return JS_NewString(ctx, "");
    std::string s = lexborToString(str, ref->node);
    return JS_NewStringLen(ctx, s.data(), s.size());
}

JSValue jsInnerHTML(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    auto* ref = thisNode(ctx, thisVal);
    if(!ref) return JS_EXCEPTION;
    lexbor_str_t str = {nullptr, 0};
    if(lxb_html_serialize_deep_str(ref->node, &str) != LXB_STATUS_OK) return JS_NewString(ctx, "");
    std::string s = lexborToString(str, ref->node);
    return JS_NewStringLen(ctx, s.data(), s.size());
}

JSValue jsInnerText(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    auto* ref = thisNode(ctx, thisVal);
    if(!ref) return JS_EXCEPTION;
    size_t len = 0;
    lxb_char_t* s = lxb_dom_node_text_content(ref->node, &len);
    if(!s) return JS_NewString(ctx, "");
    JSValue r = JS_NewStringLen(ctx, (const char*)s, len);
    lxb_dom_document_destroy_text(ref->node->owner_document, s);
    return r;
}

std::string qualifiedName(lxb_dom_node_t* node) {
    if(!node || node->type != LXB_DOM_NODE_TYPE_ELEMENT) return "";
    size_t len = 0;
    const lxb_char_t* n = lxb_dom_element_qualified_name(lxb_dom_interface_element(node), &len);
    return n ? std::string((const char*)n, len) : "";
}

JSValue jsTag(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    auto* ref = thisNode(ctx, thisVal);
    if(!ref) return JS_EXCEPTION;
    std::string n = qualifiedName(ref->node);
    return JS_NewStringLen(ctx, n.data(), n.size());
}

JSValue jsAttr(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto* ref = thisNode(ctx, thisVal);
    if(!ref) return JS_EXCEPTION;
    std::string name = argString(ctx, argc, argv, 0);
    if(ref->node->type != LXB_DOM_NODE_TYPE_ELEMENT) return JS_NewString(ctx, "");
    auto* el = lxb_dom_interface_element(ref->node);
    size_t len = 0;
    const lxb_char_t* v = lxb_dom_element_get_attribute(el, (const lxb_char_t*)name.data(), name.size(), &len);
    if(!v) return JS_NewString(ctx, "");
    return JS_NewStringLen(ctx, (const char*)v, len);
}

JSValue jsPath(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    auto* ref = thisNode(ctx, thisVal);
    if(!ref) return JS_EXCEPTION;
    StringList names;
    for(lxb_dom_node_t* p = ref->node->parent; p; p = p->parent)
        if(p->type == LXB_DOM_NODE_TYPE_ELEMENT) names.insert(names.begin(), qualifiedName(p));
    JSValue arr = JS_NewArray(ctx);
    for(uint32_t i = 0; i < names.size(); i++) JS_SetPropertyUint32(ctx, arr, i, JS_NewStringLen(ctx, names[i].data(), names[i].size()));
    return arr;
}

JSValue jsPathIds(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    auto* ref = thisNode(ctx, thisVal);
    if(!ref) return JS_EXCEPTION;
    StringList ids;
    for(lxb_dom_node_t* p = ref->node->parent; p && p->parent; p = p->parent) ids.insert(ids.begin(), std::to_string((uintptr_t)p));
    JSValue arr = JS_NewArray(ctx);
    for(uint32_t i = 0; i < ids.size(); i++) JS_SetPropertyUint32(ctx, arr, i, JS_NewStringLen(ctx, ids[i].data(), ids[i].size()));
    return arr;
}

JSValue jsParent(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    auto* ref = thisNode(ctx, thisVal);
    if(!ref) return JS_EXCEPTION;
    if(!ref->node->parent) return JS_UNDEFINED;
    return newHtmlNode(ctx, ref->doc, ref->node->parent);
}

struct CssEngine {
    lxb_css_parser_t* parser = nullptr;
    lxb_selectors_t* selectors = nullptr;
    CssEngine() {
        parser = lxb_css_parser_create();
        if(lxb_css_parser_init(parser, nullptr, nullptr) != LXB_STATUS_OK) {
            lxb_css_parser_destroy(parser, true);
            parser = nullptr;
        }
        selectors = lxb_selectors_create();
        if(lxb_selectors_init(selectors) != LXB_STATUS_OK) {
            lxb_selectors_destroy(selectors, true);
            selectors = nullptr;
        }
    }
};

CssEngine& css() {
    static CssEngine e;
    return e;
}

lxb_status_t findCallback(lxb_dom_node_t* node, lxb_css_selector_specificity_t*, void* ctx) {
    static_cast<std::vector<lxb_dom_node_t*>*>(ctx)->push_back(node);
    return LXB_STATUS_OK;
}

JSValue jsFind(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto* ref = thisNode(ctx, thisVal);
    if(!ref) return JS_EXCEPTION;
    std::string sel = argString(ctx, argc, argv, 0);
    JSValue arr = JS_NewArray(ctx);
    CssEngine& e = css();
    if(!e.parser || !e.selectors) return arr;
    lxb_css_selector_list_t* list = lxb_css_selectors_parse(e.parser, (const lxb_char_t*)sel.data(), sel.size());
    if(e.parser->status != LXB_STATUS_OK || !list) {
        log("[JavaScript] Invalid CSS selector: " + sel, LogLevel::Warning);
        if(list) lxb_css_selector_list_destroy_memory(list);
        return arr;
    }
    std::vector<lxb_dom_node_t*> nodes;
    lxb_selectors_find(e.selectors, ref->node, list, findCallback, &nodes);
    lxb_css_selector_list_destroy_memory(list);
    // A comma-separated selector list can report a node more than once
    std::unordered_set<lxb_dom_node_t*> seen;
    uint32_t i = 0;
    for(auto* n : nodes) {
        if(!seen.insert(n).second) continue;
        JS_SetPropertyUint32(ctx, arr, i++, newHtmlNode(ctx, ref->doc, n));
    }
    return arr;
}

JSValue newHtmlNode(JSContext* ctx, std::shared_ptr<HtmlDoc> doc, lxb_dom_node_t* node) {
    JSValue obj = JS_NewObjectClass(ctx, (int)g_htmlClass);
    if(JS_IsException(obj)) return obj;
    JS_SetOpaque(obj, new HtmlRef{std::move(doc), node});
    return obj;
}

void registerHtmlClass(JSContext* ctx) {
    JSRuntime* rt = JS_GetRuntime(ctx);
    if(g_htmlClass == 0) {
        JS_NewClassID(rt, &g_htmlClass);
        JSClassDef def{};
        def.class_name = "HtmlNode";
        def.finalizer = htmlFinalizer;
        JS_NewClass(rt, g_htmlClass, &def);
    }
    JSValue proto = JS_NewObject(ctx);
    struct {
        const char* name;
        JSCFunction* fn;
        int len;
    } methods[] = {{"outerHTML", jsOuterHTML, 0}, {"innerHTML", jsInnerHTML, 0}, {"innerText", jsInnerText, 0},
                   {"tag", jsTag, 0},             {"attr", jsAttr, 1},           {"path", jsPath, 0},
                   {"pathIds", jsPathIds, 0},     {"parent", jsParent, 0},       {"find", jsFind, 1}};
    for(auto& m : methods) JS_SetPropertyStr(ctx, proto, m.name, JS_NewCFunction(ctx, m.fn, m.name, m.len));
    JS_SetClassProto(ctx, g_htmlClass, proto);
}

JSValue jsParseHTML(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string html = argString(ctx, argc, argv, 0);
    bool fragment = argc > 1 && JS_ToBool(ctx, argv[1]) > 0;
    auto doc = std::make_shared<HtmlDoc>();
    doc->doc = lxb_html_document_create();
    if(!doc->doc) return JS_UNDEFINED;
    if(fragment) {
        lxb_html_element_t* root = lxb_html_document_create_element(doc->doc, (const lxb_char_t*)"p", 1, nullptr);
        lxb_dom_node_t* node = lxb_html_document_parse_fragment(doc->doc, &root->element, (const lxb_char_t*)html.data(), html.size());
        if(!node) {
            log("[JavaScript] Error parsing HTML fragment", LogLevel::Error);
            return JS_UNDEFINED;
        }
        if(node->first_child && node->first_child == node->last_child) node = node->first_child;
        return newHtmlNode(ctx, doc, node);
    }
    if(lxb_html_document_parse(doc->doc, (const lxb_char_t*)html.data(), html.size()) != LXB_STATUS_OK) {
        log("[JavaScript] Error parsing HTML", LogLevel::Error);
        return JS_UNDEFINED;
    }
    lxb_html_body_element_t* body = lxb_html_document_body_element(doc->doc);
    if(!body) return JS_UNDEFINED;
    return newHtmlNode(ctx, doc, lxb_dom_interface_node(body));
}

// ---- XML (libxml2) -----------------------------------------------------------
// Mirrors the QDomDocument conversion of the original: elements become objects, attributes go
// into "@attributes", text into "#text", repeated children become arrays.

bool isBlank(const xmlChar* s) {
    if(!s) return true;
    for(; *s; s++)
        if(!g_ascii_isspace(*s)) return false;
    return true;
}

JSValue xmlToJs(JSContext* ctx, xmlNode* node) {
    if(node->type == XML_TEXT_NODE || node->type == XML_CDATA_SECTION_NODE) {
        const char* c = (const char*)node->content;
        return JS_NewString(ctx, c ? c : "");
    }
    JSValue obj = JS_NewObject(ctx);
    if(node->type == XML_ELEMENT_NODE && node->properties) {
        JSValue attrs = JS_NewObject(ctx);
        for(xmlAttr* a = node->properties; a; a = a->next) {
            xmlChar* v = xmlNodeListGetString(node->doc, a->children, 1);
            std::string name = a->ns && a->ns->prefix ? std::string((const char*)a->ns->prefix) + ":" + (const char*)a->name : (const char*)a->name;
            JS_SetPropertyStr(ctx, attrs, name.c_str(), JS_NewString(ctx, v ? (const char*)v : ""));
            xmlFree(v);
        }
        JS_SetPropertyStr(ctx, obj, "@attributes", attrs);
    }
    for(xmlNode* c = node->children; c; c = c->next) {
        std::string name;
        if(c->type == XML_TEXT_NODE) {
            if(isBlank(c->content)) continue;
            name = "#text";
        } else if(c->type == XML_CDATA_SECTION_NODE) {
            name = "#cdata-section";
        } else if(c->type == XML_COMMENT_NODE) {
            name = "#comment";
        } else if(c->type == XML_ELEMENT_NODE) {
            name = c->ns && c->ns->prefix ? std::string((const char*)c->ns->prefix) + ":" + (const char*)c->name : (const char*)c->name;
        } else {
            continue;
        }
        JSValue child = xmlToJs(ctx, c);
        JSValue existing = JS_GetPropertyStr(ctx, obj, name.c_str());
        if(JS_IsUndefined(existing)) {
            JS_SetPropertyStr(ctx, obj, name.c_str(), child);
        } else if(JS_IsArray(existing)) {
            int64_t len = 0;
            JSValue l = JS_GetPropertyStr(ctx, existing, "length");
            JS_ToInt64(ctx, &len, l);
            JS_FreeValue(ctx, l);
            JS_SetPropertyUint32(ctx, existing, (uint32_t)len, child);
        } else {
            JSValue arr = JS_NewArray(ctx);
            JS_SetPropertyUint32(ctx, arr, 0, JS_DupValue(ctx, existing));
            JS_SetPropertyUint32(ctx, arr, 1, child);
            JS_SetPropertyStr(ctx, obj, name.c_str(), arr);
        }
        JS_FreeValue(ctx, existing);
    }
    return obj;
}

JSValue jsParseXML(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string xml = argString(ctx, argc, argv, 0);
    xmlDoc* doc = xmlReadMemory(xml.data(), (int)xml.size(), "source.xml", nullptr,
                                XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_RECOVER);
    if(!doc) {
        log("[JavaScript] Error parsing XML document", LogLevel::Error);
        return JS_UNDEFINED;
    }
    JSValue r = xmlToJs(ctx, reinterpret_cast<xmlNode*>(doc));
    xmlFreeDoc(doc);
    return r;
}

// ---- Grabber helpers -----------------------------------------------------------

JSValue jsHtmlDecode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string s = decodeHtmlEntities(argString(ctx, argc, argv, 0));
    return JS_NewStringLen(ctx, s.data(), s.size());
}

JSValue jsRegexMatches(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string pattern = argString(ctx, argc, argv, 0);
    std::string txt = argString(ctx, argc, argv, 1);
    JSValue arr = JS_NewArray(ctx);
    Regex rx(pattern, false, true);
    if(!rx.valid()) {
        log("[JavaScript] Invalid regular expression: " + rx.error(), LogLevel::Warning);
        return arr;
    }
    uint32_t idx = 0;
    for(auto& m : rx.matchAll(txt)) {
        JSValue obj = JS_NewObject(ctx);
        for(auto& [group, val] : m.named) {
            if(val.empty()) continue;
            std::string name = group;
            size_t us = name.rfind('_');
            bool ok = false;
            if(us != std::string::npos) toInt(name.substr(us + 1), 0, &ok);
            if(ok) name = name.substr(0, us);
            JS_SetPropertyStr(ctx, obj, name.c_str(), JS_NewStringLen(ctx, val.data(), val.size()));
        }
        for(uint32_t i = 0; i < m.groups.size(); i++) JS_SetPropertyUint32(ctx, obj, i, JS_NewStringLen(ctx, m.groups[i].data(), m.groups[i].size()));
        JS_SetPropertyUint32(ctx, arr, idx++, obj);
    }
    return arr;
}

JSValue jsConsole(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic) {
    std::string msg;
    for(int i = 0; i < argc; i++) {
        if(i) msg += " ";
        msg += toStdString(ctx, argv[i]);
    }
    static const LogLevel levels[] = {LogLevel::Debug, LogLevel::Error, LogLevel::Info, LogLevel::Info, LogLevel::Warning};
    log("[JavaScript] " + msg, levels[magic]);
    return JS_UNDEFINED;
}

void installNatives(JSContext* ctx) {
    registerHtmlClass(ctx);
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue grabber = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, grabber, "htmlDecode", JS_NewCFunction(ctx, jsHtmlDecode, "htmlDecode", 1));
    JS_SetPropertyStr(ctx, grabber, "regexMatches", JS_NewCFunction(ctx, jsRegexMatches, "regexMatches", 2));
    JS_SetPropertyStr(ctx, grabber, "parseXML", JS_NewCFunction(ctx, jsParseXML, "parseXML", 1));
    JS_SetPropertyStr(ctx, grabber, "parseHTML", JS_NewCFunction(ctx, jsParseHTML, "parseHTML", 2));
    JS_SetPropertyStr(ctx, global, "Grabber", grabber);
    JSValue console = JS_NewObject(ctx);
    const char* names[] = {"debug", "error", "info", "log", "warn"};
    for(int i = 0; i < 5; i++) JS_SetPropertyStr(ctx, console, names[i], JS_NewCFunctionMagic(ctx, jsConsole, names[i], 1, JS_CFUNC_generic_magic, i));
    JS_SetPropertyStr(ctx, global, "console", console);
    JS_FreeValue(ctx, global);
}
}  // namespace

// ---- Context -----------------------------------------------------------------

Context::Context(const std::string& helperFile) {
    m_ctx = JS_NewContext(runtime());
    installNatives(m_ctx);
    if(!helperFile.empty()) {
        auto code = readFile(helperFile);
        if(!code) {
            log("JavaScript helper file could not be opened: " + helperFile, LogLevel::Error);
        } else {
            Value r = eval(*code, helperFile);
            if(r.isError()) log(r.errorMessage(), LogLevel::Error);
        }
    }
}

Context::~Context() {
    if(m_ctx) JS_FreeContext(m_ctx);
}

Value Context::eval(const std::string& code, const std::string& filename) {
    Deadline d(15000);
    // JS_Eval requires a NUL terminated buffer
    JSValue r = JS_Eval(m_ctx, code.c_str(), code.size(), filename.c_str(), JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(r)) return wrap(m_ctx, JS_GetException(m_ctx), true);
    return wrap(m_ctx, r, JS_IsError(r));
}

Value Context::global() const { return wrap(m_ctx, JS_GetGlobalObject(m_ctx)); }
Value Context::undefined() const { return wrap(m_ctx, JS_UNDEFINED); }
Value Context::null() const { return wrap(m_ctx, JS_NULL); }
Value Context::newObject() const { return wrap(m_ctx, JS_NewObject(m_ctx)); }
Value Context::newArray() const { return wrap(m_ctx, JS_NewArray(m_ctx)); }
Value Context::newString(const std::string& s) const { return wrap(m_ctx, JS_NewStringLen(m_ctx, s.data(), s.size())); }
Value Context::newNumber(double d) const { return wrap(m_ctx, JS_NewFloat64(m_ctx, d)); }
Value Context::newInt(long long i) const { return wrap(m_ctx, JS_NewInt64(m_ctx, i)); }
Value Context::newBool(bool b) const { return wrap(m_ctx, JS_NewBool(m_ctx, b)); }

Value Context::fromStringList(const StringList& l) const {
    Value a = newArray();
    for(uint32_t i = 0; i < l.size(); i++) a.setAt(i, newString(l[i]));
    return a;
}

Value Context::fromStringMap(const StringMap& m) const {
    Value o = newObject();
    for(auto& [k, v] : m) o.set(k, v);
    return o;
}

Value Context::fromJson(const nlohmann::json& j) const {
    switch(j.type()) {
        case nlohmann::json::value_t::null: return null();
        case nlohmann::json::value_t::boolean: return newBool(j.get<bool>());
        case nlohmann::json::value_t::number_integer:
        case nlohmann::json::value_t::number_unsigned: return newInt(j.get<long long>());
        case nlohmann::json::value_t::number_float: return newNumber(j.get<double>());
        case nlohmann::json::value_t::string: return newString(j.get<std::string>());
        case nlohmann::json::value_t::array: {
            Value a = newArray();
            uint32_t i = 0;
            for(auto& e : j) a.setAt(i++, fromJson(e));
            return a;
        }
        case nlohmann::json::value_t::object: {
            Value o = newObject();
            for(auto it = j.begin(); it != j.end(); ++it) o.set(it.key(), fromJson(it.value()));
            return o;
        }
        default: return undefined();
    }
}

void setHelperFile(const std::string& path) { g_helperFile = path; }

Context& sourceContext() {
    static Context* ctx = nullptr;  // intentionally never freed: models keep values alive until exit
    if(!ctx) ctx = new Context(g_helperFile);
    return *ctx;
}

Value evalWithVariables(Context& ctx, const std::string& code) { return ctx.eval(code); }

StringMap htmlInputValues(const std::string& html, const StringList& names) {
    StringMap out;
    HtmlDoc doc;
    doc.doc = lxb_html_document_create();
    if(!doc.doc || lxb_html_document_parse(doc.doc, (const lxb_char_t*)html.data(), html.size()) != LXB_STATUS_OK) return out;
    CssEngine& e = css();
    if(!e.parser || !e.selectors) return out;
    lxb_dom_node_t* root = lxb_dom_interface_node(doc.doc);
    for(auto& name : names) {
        std::string sel = "input[name=\"" + name + "\"]";
        lxb_css_selector_list_t* list = lxb_css_selectors_parse(e.parser, (const lxb_char_t*)sel.data(), sel.size());
        if(!list) continue;
        std::vector<lxb_dom_node_t*> nodes;
        lxb_selectors_find(e.selectors, root, list, findCallback, &nodes);
        lxb_css_selector_list_destroy_memory(list);
        if(nodes.empty()) continue;
        size_t len = 0;
        const lxb_char_t* v = lxb_dom_element_get_attribute(lxb_dom_interface_element(nodes[0]), (const lxb_char_t*)"value", 5, &len);
        out[name] = v ? std::string((const char*)v, len) : "";
    }
    return out;
}

}  // namespace bg::js
