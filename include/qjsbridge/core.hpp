// SPDX-License-Identifier: MIT
// qjsbridge – RAII wrappers for JSRuntime, JSContext, JSValue + exception types
#pragma once

// ── QuickJS include path ──────────────────────────────────────────────────────
// Define QJSBRIDGE_QUICKJS_NG before including this header when using
// QuickJS-ng (changes JS_NewClassID signature from 1-arg to 2-arg).
#ifdef QJSBRIDGE_QUICKJS_NG
#  include <quickjs.h>
#else
#  include <quickjs/quickjs.h>
#endif

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace qjsb {

// ── Exception types ───────────────────────────────────────────────────────────

/// Base for all qjsbridge exceptions.
class Exception : public std::runtime_error {
public:
    explicit Exception(const std::string& msg) : std::runtime_error(msg) {}
    explicit Exception(const char* msg) : std::runtime_error(msg) {}
};

/// Thrown when a pending JS exception is propagated to C++.
class JSException : public Exception {
    std::string stack_;
public:
    JSException(const std::string& msg, std::string stack = {})
        : Exception(msg), stack_(std::move(stack)) {}
    const std::string& stack() const noexcept { return stack_; }
};

/// Thrown on a type mismatch during value conversion.
class TypeError : public Exception {
public:
    explicit TypeError(const std::string& msg) : Exception("TypeError: " + msg) {}
};

/// High-level configurable QuickJS module loader.
///
/// Typical usage:
///   1) configure normalize/source/file-read hooks on ModuleLoader
///   2) install with Runtime::setModuleLoader(ModuleLoader)
///   3) evaluate module code via Context::evalModule(...)
///
/// Compared with Runtime::setModuleLoader(JSModuleNormalizeFunc*, ...),
/// this API is C++-friendly (std::function hooks) and keeps loader lifetime
/// tied to Runtime to avoid dangling opaque pointers.
class ModuleLoader {
public:
    using NormalizeFn = std::function<std::string(
        JSContext* ctx,
        const std::string& module_base_name,
        const std::string& module_name)>;

    using SourceLoaderFn = std::function<std::optional<std::string>(
        JSContext* ctx,
        const std::string& normalized_name)>;

    using FileReaderFn = std::function<std::optional<std::string>(
        const std::string& path)>;

private:
    NormalizeFn normalize_;
    SourceLoaderFn source_loader_;
    FileReaderFn file_reader_;
    std::vector<std::string> search_paths_;
    // "" means "try module name as-is (no extension)".
    std::vector<std::string> extensions_{"", ".js", ".mjs"};

public:
    ModuleLoader() = default;

    ModuleLoader& setNormalize(NormalizeFn fn) {
        normalize_ = std::move(fn);
        return *this;
    }

    ModuleLoader& setSourceLoader(SourceLoaderFn fn) {
        source_loader_ = std::move(fn);
        return *this;
    }

    ModuleLoader& setFileReader(FileReaderFn fn) {
        file_reader_ = std::move(fn);
        return *this;
    }

    ModuleLoader& addSearchPath(std::string path) {
        search_paths_.push_back(std::move(path));
        return *this;
    }

    ModuleLoader& clearSearchPaths() {
        search_paths_.clear();
        return *this;
    }

    ModuleLoader& setExtensions(std::vector<std::string> exts) {
        extensions_ = std::move(exts);
        if (extensions_.empty())
            extensions_.push_back("");
        return *this;
    }

    const NormalizeFn& normalize() const noexcept { return normalize_; }
    const SourceLoaderFn& sourceLoader() const noexcept { return source_loader_; }
    const FileReaderFn& fileReader() const noexcept { return file_reader_; }
    const std::vector<std::string>& searchPaths() const noexcept { return search_paths_; }
    const std::vector<std::string>& extensions() const noexcept { return extensions_; }
};

// ── JS_NewClassID compatibility shim ─────────────────────────────────────────
// Original QuickJS:  JS_NewClassID(JSClassID *pclass_id)
// QuickJS-ng:        JS_NewClassID(JSRuntime *rt, JSClassID *pclass_id)

namespace detail {
#ifdef QJSBRIDGE_QUICKJS_NG
    inline JSClassID qjsb_new_class_id(JSRuntime* rt, JSClassID* id) {
        return JS_NewClassID(rt, id);
    }
#else
    inline JSClassID qjsb_new_class_id(JSRuntime* /*rt*/, JSClassID* id) {
        return JS_NewClassID(id);
    }
#endif

inline std::string default_module_normalize_(const std::string& base_module_path,
                                             const std::string& module_name) {
    namespace fs = std::filesystem;
    fs::path name(module_name);
    if (module_name.rfind("./", 0) == 0 || module_name.rfind("../", 0) == 0) {
        fs::path base(base_module_path);
        fs::path base_dir = base.has_parent_path() ? base.parent_path() : fs::path(".");
        return (base_dir / name).lexically_normal().generic_string();
    }
    return name.lexically_normal().generic_string();
}

inline std::optional<std::string> default_read_file_(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
        return std::nullopt;
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    return content;
}

inline std::optional<std::string> module_loader_read_source_(
    const ModuleLoader& loader,
    const std::string& module_name) {
    namespace fs = std::filesystem;
    std::vector<fs::path> candidates;
    fs::path requested(module_name);

    auto append_with_extensions = [&](const fs::path& p) {
        if (p.has_extension()) {
            candidates.push_back(p);
            return;
        }
        for (const auto& ext : loader.extensions()) {
            if (ext.empty()) candidates.push_back(p);
            else candidates.push_back(p.string() + ext);
        }
    };

    append_with_extensions(requested);
    for (const auto& root : loader.searchPaths()) {
        append_with_extensions(fs::path(root) / requested);
    }

    for (const auto& candidate : candidates) {
        std::optional<std::string> src;
        if (loader.fileReader()) {
            src = loader.fileReader()(candidate.generic_string());
        } else {
            src = default_read_file_(candidate.generic_string());
        }
        if (src) return src;
    }
    return std::nullopt;
}

inline char* module_loader_normalize_dispatch_(JSContext* ctx,
                                               const char* module_base_name,
                                               const char* module_name,
                                               void* opaque) {
    auto* loader = static_cast<ModuleLoader*>(opaque);
    if (!loader || !module_name)
        return nullptr;

    std::string normalized;
    try {
        if (loader->normalize()) {
            normalized = loader->normalize()(
                ctx,
                module_base_name ? module_base_name : "",
                module_name);
        } else {
            normalized = default_module_normalize_(
                module_base_name ? module_base_name : "",
                module_name);
        }
    } catch (const std::exception& e) {
        JS_ThrowInternalError(ctx, "module normalize failed: %s", e.what());
        return nullptr;
    } catch (...) {
        JS_ThrowInternalError(ctx, "module normalize failed");
        return nullptr;
    }

    char* out = static_cast<char*>(js_malloc(ctx, normalized.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, normalized.c_str(), normalized.size());
    out[normalized.size()] = '\0';
    return out;
}

inline JSModuleDef* module_loader_dispatch_(JSContext* ctx,
                                            const char* module_name,
                                            void* opaque) {
    auto* loader = static_cast<ModuleLoader*>(opaque);
    if (!loader || !module_name)
        return nullptr;

    std::optional<std::string> source;
    try {
        if (loader->sourceLoader()) {
            source = loader->sourceLoader()(ctx, module_name);
        } else {
            source = module_loader_read_source_(*loader, module_name);
        }
    } catch (const std::exception& e) {
        JS_ThrowInternalError(ctx, "module loader failed: %s", e.what());
        return nullptr;
    } catch (...) {
        JS_ThrowInternalError(ctx, "module loader failed");
        return nullptr;
    }

    if (!source) {
        JS_ThrowReferenceError(
            ctx,
            "Could not load module '%s' (no source loader configured or source loader returned empty, and filesystem lookup failed)",
            module_name);
        return nullptr;
    }

    JSValue func_val = JS_Eval(ctx,
                               source->c_str(),
                               source->size(),
                               module_name,
                               JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(func_val))
        return nullptr;

    if (JS_VALUE_GET_TAG(func_val) != JS_TAG_MODULE) {
        JS_FreeValue(ctx, func_val);
        JS_ThrowTypeError(ctx, "Loaded script is not a module: '%s'", module_name);
        return nullptr;
    }

    return static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(func_val));
}

struct ModuleExport {
    std::string name;
    std::function<JSValue(JSContext*)> factory;
};

struct ModuleState {
    JSContext* ctx{nullptr};
    std::vector<ModuleExport> exports;
    std::vector<JSValue> retained;
};

inline std::unordered_map<JSModuleDef*, std::shared_ptr<ModuleState>>& module_states() {
    static std::unordered_map<JSModuleDef*, std::shared_ptr<ModuleState>> m;
    return m;
}

inline int module_init_dispatch(JSContext* ctx, JSModuleDef* m) {
    auto it = module_states().find(m);
    if (it == module_states().end())
        return -1;
    const auto& state = it->second;
    for (const auto& exp : state->exports) {
        JSValue v = exp.factory(ctx);
        if (JS_IsException(v))
            return -1;
        if (JS_SetModuleExport(ctx, m, exp.name.c_str(), v) < 0) {
            JS_FreeValue(ctx, v);
            return -1;
        }
    }
    return 0;
}
} // namespace detail

// ── Runtime ───────────────────────────────────────────────────────────────────

/// RAII owner of a JSRuntime.
class Runtime {
    JSRuntime* rt_;
    std::shared_ptr<ModuleLoader> module_loader_;
public:
    Runtime() : rt_(JS_NewRuntime()) {
        if (!rt_) throw Exception("Failed to create JSRuntime");
    }
    ~Runtime() noexcept { if (rt_) JS_FreeRuntime(rt_); }

    Runtime(const Runtime&)            = delete;
    Runtime& operator=(const Runtime&) = delete;

    Runtime(Runtime&& o) noexcept : rt_(o.rt_) { o.rt_ = nullptr; }
    Runtime& operator=(Runtime&& o) noexcept {
        if (this != &o) {
            if (rt_) JS_FreeRuntime(rt_);
            rt_ = o.rt_;
            o.rt_ = nullptr;
        }
        return *this;
    }

    JSRuntime* get() const noexcept { return rt_; }

    void setMemoryLimit(size_t limit) noexcept { JS_SetMemoryLimit(rt_, limit); }
    void setMaxStackSize(size_t size)  noexcept { JS_SetMaxStackSize(rt_, size); }
    void runGC() noexcept { JS_RunGC(rt_); }
    void setModuleLoader(JSModuleNormalizeFunc* normalize,
                         JSModuleLoaderFunc* loader,
                         void* opaque = nullptr) noexcept {
        module_loader_.reset();
        JS_SetModuleLoaderFunc(rt_, normalize, loader, opaque);
    }

    void setModuleLoader(ModuleLoader loader) {
        module_loader_ = std::make_shared<ModuleLoader>(std::move(loader));
        JS_SetModuleLoaderFunc(rt_,
                               detail::module_loader_normalize_dispatch_,
                               detail::module_loader_dispatch_,
                               module_loader_.get());
    }
};

// ── Value ─────────────────────────────────────────────────────────────────────

/// RAII owning wrapper for a JSValue.
///
/// Constructing from (JSContext*, JSValue) **takes ownership** – the caller
/// must not free the JSValue after passing it in.
/// Use Value::dup() to create a reference-counted copy.
class Value {
    JSContext* ctx_;
    JSValue    val_;

    void free_() noexcept {
        if (ctx_) JS_FreeValue(ctx_, val_);
    }

public:
    // Takes ownership (no dup).
    Value(JSContext* ctx, JSValue val) noexcept : ctx_(ctx), val_(val) {}

    // Creates a reference-counted duplicate.
    static Value dup(JSContext* ctx, JSValueConst val) {
        return Value(ctx, JS_DupValue(ctx, val));
    }

    ~Value() noexcept { free_(); }

    // Copy: dup the underlying JS value.
    Value(const Value& o)
        : ctx_(o.ctx_),
          val_(o.ctx_ ? JS_DupValue(o.ctx_, o.val_) : JS_UNDEFINED) {}

    Value& operator=(const Value& o) {
        if (this != &o) {
            free_();
            ctx_ = o.ctx_;
            val_ = o.ctx_ ? JS_DupValue(o.ctx_, o.val_) : JS_UNDEFINED;
        }
        return *this;
    }

    // Move: steal without dup.
    Value(Value&& o) noexcept : ctx_(o.ctx_), val_(o.val_) {
        o.ctx_ = nullptr;
        o.val_ = JS_UNDEFINED;
    }
    Value& operator=(Value&& o) noexcept {
        if (this != &o) {
            free_();
            ctx_ = o.ctx_;
            val_ = o.val_;
            o.ctx_ = nullptr;
            o.val_ = JS_UNDEFINED;
        }
        return *this;
    }

    JSValue    get()  const noexcept { return val_; }
    JSContext* ctx()  const noexcept { return ctx_; }

    /// Release ownership: returns the raw JSValue, caller is responsible for
    /// calling JS_FreeValue.
    JSValue steal() noexcept {
        JSValue v = val_;
        ctx_ = nullptr;
        val_ = JS_UNDEFINED;
        return v;
    }

    // ── Type predicates ───────────────────────────────────────────────────────
    bool isUndefined() const noexcept { return JS_IsUndefined(val_); }
    bool isNull()      const noexcept { return JS_IsNull(val_); }
    bool isBool()      const noexcept { return JS_IsBool(val_); }
    bool isNumber()    const noexcept { return JS_IsNumber(val_); }
    bool isString()    const noexcept { return JS_IsString(val_); }
    bool isObject()    const noexcept { return JS_IsObject(val_); }
    bool isArray()     const noexcept { return ctx_ && JS_IsArray(ctx_, val_) == 1; }
    bool isFunction()  const noexcept { return ctx_ && JS_IsFunction(ctx_, val_); }
    bool isException() const noexcept { return JS_IsException(val_); }
    bool isSymbol()    const noexcept { return JS_IsSymbol(val_); }

    // ── String conversion (calls JS toString on any value) ────────────────────
    std::string toString() const {
        if (!ctx_) return {};
        JSValue s = JS_ToString(ctx_, val_);
        if (JS_IsException(s)) { JS_FreeValue(ctx_, s); return "<exception>"; }
        size_t len;
        const char* p = JS_ToCStringLen(ctx_, &len, s);
        std::string result;
        if (p) { result.assign(p, len); JS_FreeCString(ctx_, p); }
        JS_FreeValue(ctx_, s);
        return result;
    }

    // ── Property access ───────────────────────────────────────────────────────
    Value operator[](const char* name) const {
        return Value(ctx_, JS_GetPropertyStr(ctx_, val_, name));
    }
    Value operator[](const std::string& name) const { return (*this)[name.c_str()]; }
    Value operator[](uint32_t idx) const {
        return Value(ctx_, JS_GetPropertyUint32(ctx_, val_, idx));
    }

    bool set(const char* name, Value v) {
        return JS_SetPropertyStr(ctx_, val_, name, v.steal()) >= 0;
    }
    bool set(const std::string& name, Value v) { return set(name.c_str(), std::move(v)); }
    bool set(uint32_t idx, Value v) {
        return JS_SetPropertyUint32(ctx_, val_, idx, v.steal()) >= 0;
    }

    // ── Array length ──────────────────────────────────────────────────────────
    int64_t length() const {
        Value lv(ctx_, JS_GetPropertyStr(ctx_, val_, "length"));
        int64_t n = 0;
        JS_ToInt64(ctx_, &n, lv.get());
        return n;
    }

    // ── Calling ───────────────────────────────────────────────────────────────
    Value call(JSValueConst this_val, int argc = 0, JSValue* argv = nullptr) const {
        return Value(ctx_, JS_Call(ctx_, val_, this_val, argc, argv));
    }
    Value call(int argc = 0, JSValue* argv = nullptr) const {
        return call(JS_UNDEFINED, argc, argv);
    }
    Value callAsConstructor(int argc = 0, JSValue* argv = nullptr) const {
        return Value(ctx_, JS_CallConstructor(ctx_, val_, argc, argv));
    }
};

// ── Pending-exception helpers ─────────────────────────────────────────────────

namespace detail {

inline std::string extractExceptionMessage(JSContext* ctx) {
    JSValue exc = JS_GetException(ctx);

    // message string
    JSValue ms = JS_ToString(ctx, exc);
    std::string msg;
    if (!JS_IsException(ms)) {
        size_t len;
        const char* p = JS_ToCStringLen(ctx, &len, ms);
        if (p) { msg.assign(p, len); JS_FreeCString(ctx, p); }
    }
    JS_FreeValue(ctx, ms);

    // optional stack trace
    std::string stack;
    JSValue sv = JS_GetPropertyStr(ctx, exc, "stack");
    if (!JS_IsException(sv) && !JS_IsUndefined(sv)) {
        size_t len;
        const char* p = JS_ToCStringLen(ctx, &len, sv);
        if (p) { stack.assign(p, len); JS_FreeCString(ctx, p); }
    }
    JS_FreeValue(ctx, sv);
    JS_FreeValue(ctx, exc);
    return stack.empty() ? msg : msg + "\n" + stack;
}

} // namespace detail

/// Fetch and throw the pending JS exception as a C++ JSException.
[[noreturn]] inline void throwJSException(JSContext* ctx) {
    throw JSException(detail::extractExceptionMessage(ctx));
}

class Module;
using RawFunctionCallback = std::function<JSValue(
    JSContext*,
    JSValueConst,
    int,
    JSValueConst*)>;

// ── Context ───────────────────────────────────────────────────────────────────

/// RAII owner of a JSContext.  Provides eval, global-variable access, and
/// convenience wrappers for binding C++ callables.
class Context {
    JSContext* ctx_;

    void check_(JSValue v) const {
        if (JS_IsException(v)) {
            JS_FreeValue(ctx_, v);
            throwJSException(ctx_);
        }
    }

public:
    explicit Context(Runtime& rt) : ctx_(JS_NewContext(rt.get())) {
        if (!ctx_) throw Exception("Failed to create JSContext");
    }
    explicit Context(JSRuntime* rt) : ctx_(JS_NewContext(rt)) {
        if (!ctx_) throw Exception("Failed to create JSContext");
    }
    ~Context() noexcept { if (ctx_) JS_FreeContext(ctx_); }

    Context(const Context&)            = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&& o) noexcept : ctx_(o.ctx_) { o.ctx_ = nullptr; }

    JSContext* get() const noexcept { return ctx_; }

    // ── Code evaluation ───────────────────────────────────────────────────────

    /// Evaluate a JS string; throws JSException on error.
    Value eval(const std::string& code,
               const std::string& filename = "<eval>",
               int flags = JS_EVAL_TYPE_GLOBAL) {
        JSValue v = JS_Eval(ctx_, code.c_str(), code.size(),
                            filename.c_str(), flags);
        check_(v);
        return Value(ctx_, v);
    }

    /// Evaluate JS module code; supports `import` / `export`.
    Value evalModule(const std::string& code,
                     const std::string& filename = "<module>") {
        return eval(code, filename, JS_EVAL_TYPE_MODULE);
    }

    /// Read and evaluate a file; throws on I/O or JS error.
    Value evalFile(const std::string& filename) {
        FILE* f = fopen(filename.c_str(), "rb");
        if (!f) throw Exception("Cannot open file: " + filename);
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        std::string buf(static_cast<size_t>(sz), '\0');
        if (sz > 0) {
            size_t rd = fread(&buf[0], 1, static_cast<size_t>(sz), f);
            (void)rd;
        }
        fclose(f);
        return eval(buf, filename);
    }

    // ── Global object access ──────────────────────────────────────────────────
    Value globalObject() { return Value(ctx_, JS_GetGlobalObject(ctx_)); }

    Value getGlobal(const std::string& name) {
        Value g(ctx_, JS_GetGlobalObject(ctx_));
        return g[name.c_str()];
    }

    void setGlobal(const std::string& name, Value val) {
        JSValue g = JS_GetGlobalObject(ctx_);
        JS_SetPropertyStr(ctx_, g, name.c_str(), val.steal());
        JS_FreeValue(ctx_, g);
    }

    // ── Object / array factories ──────────────────────────────────────────────
    Value newObject() { return Value(ctx_, JS_NewObject(ctx_)); }
    Value newArray()  { return Value(ctx_, JS_NewArray(ctx_)); }

    // ── Function binding (implemented in function.hpp) ────────────────────────
    template <typename Fn>
    void bindFunction(const std::string& name, Fn&& fn, int length = -1);
    void bindFunctionRaw(const std::string& name,
                         RawFunctionCallback fn,
                         int length = -1);

    /// Create a C module object for exporting C++ bindings to JS modules.
    Module newModule(const std::string& name);

    JSContext* operator->() const noexcept { return ctx_; }
};

/// Builder/holder for a QuickJS C module (`JS_NewCModule`).
/// Use with Context::evalModule and JS `import { ... } from "name"`.
class Module {
    JSContext* ctx_{nullptr};
    JSModuleDef* mod_{nullptr};
    std::shared_ptr<detail::ModuleState> state_;

    void cleanup_() noexcept {
        if (!state_)
            return;
        for (JSValue v : state_->retained) {
            JS_FreeValue(state_->ctx, v);
        }
        if (mod_) {
            detail::module_states().erase(mod_);
        }
        state_.reset();
        mod_ = nullptr;
        ctx_ = nullptr;
    }

public:
    Module() = default;

    Module(JSContext* ctx, const std::string& name) : ctx_(ctx) {
        state_ = std::make_shared<detail::ModuleState>();
        state_->ctx = ctx;
        mod_ = JS_NewCModule(ctx_, name.c_str(), detail::module_init_dispatch);
        if (!mod_)
            throw Exception("Failed to create module: " + name);
        detail::module_states()[mod_] = state_;
    }

    explicit Module(Context& ctx, const std::string& name) : Module(ctx.get(), name) {}

    ~Module() { cleanup_(); }

    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;
    Module(Module&& o) noexcept
        : ctx_(o.ctx_), mod_(o.mod_), state_(std::move(o.state_)) {
        o.ctx_ = nullptr;
        o.mod_ = nullptr;
    }
    Module& operator=(Module&& o) noexcept {
        if (this != &o) {
            cleanup_();
            ctx_ = o.ctx_;
            mod_ = o.mod_;
            state_ = std::move(o.state_);
            o.ctx_ = nullptr;
            o.mod_ = nullptr;
        }
        return *this;
    }

    JSContext* context() const noexcept { return ctx_; }
    JSModuleDef* get() const noexcept { return mod_; }

    /// Add an already-created JS value as module export.
    /// The value is retained until this Module is destroyed.
    Module& exportValue(const std::string& name, Value v) {
        if (!mod_) throw Exception("Invalid module");
        if (JS_AddModuleExport(ctx_, mod_, name.c_str()) < 0)
            throwJSException(ctx_);
        JSValue retained = JS_DupValue(ctx_, v.get());
        state_->retained.push_back(retained);
        state_->exports.push_back(detail::ModuleExport{
            name,
            [retained](JSContext* ctx) {
                return JS_DupValue(ctx, retained);
            }
        });
        return *this;
    }

    template <typename Fn>
    Module& bindFunction(const std::string& name, Fn&& fn, int length = -1);
    Module& bindFunctionRaw(const std::string& name,
                            RawFunctionCallback fn,
                            int length = -1);
};

inline Module Context::newModule(const std::string& name) {
    return Module(ctx_, name);
}

} // namespace qjsb
