// SPDX-License-Identifier: Apache-2.0
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
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace qjsb {

template <typename T>
class ClassDef;
template <typename T, typename Enable>
struct Converter;

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

/// quickjspp-compatible module loader result.
struct ModuleData {
    std::optional<std::string> source;
    std::optional<std::string> url;

    ModuleData() = default;
    explicit ModuleData(std::optional<std::string> source_)
        : source(std::move(source_)) {}
    ModuleData(std::optional<std::string> url_,
               std::optional<std::string> source_)
        : source(std::move(source_)), url(std::move(url_)) {}
};

/// quickjspp-compatible module loader callback.
using ModuleLoader = std::function<ModuleData(std::string_view)>;

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

inline std::optional<std::string> readFile(const std::filesystem::path& filepath) {
    if (!std::filesystem::exists(filepath))
        return std::nullopt;
    std::ifstream in(filepath, std::ios::in | std::ios::binary);
    if (!in.is_open())
        return std::nullopt;
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    return content;
}

inline std::string toUri(std::string_view filename) {
    auto name = std::string(filename);
    const auto scheme_pos = name.find("://");
    const auto slash_pos = name.find('/');
    if (scheme_pos != std::string::npos &&
        (slash_pos == std::string::npos || scheme_pos < slash_pos)) {
        return name;
    }

    auto path = std::filesystem::path(name);
    if (!path.is_absolute()) {
        path = std::filesystem::current_path() / path;
    }
    path = std::filesystem::weakly_canonical(path);
    return "file://" + path.generic_string();
}

inline ModuleLoader default_module_loader_() {
    return [](std::string_view filename) -> ModuleData {
        return ModuleData{toUri(filename), readFile(std::string(filename))};
    };
}

inline JSModuleDef* module_loader_dispatch_(JSContext* ctx,
                                            const char* module_name,
                                            void* opaque);

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
public:
    Runtime() : rt_(JS_NewRuntime()) {
        if (!rt_) throw Exception("Failed to create JSRuntime");
        JS_SetModuleLoaderFunc(rt_, nullptr, detail::module_loader_dispatch_, nullptr);
    }
    ~Runtime() noexcept { if (rt_) JS_FreeRuntime(rt_); }

    Runtime(const Runtime&)            = delete;
    Runtime& operator=(const Runtime&) = delete;

    Runtime(Runtime&& o) noexcept
        : rt_(o.rt_) {
        o.rt_ = nullptr;
    }
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
};

// ── Value ─────────────────────────────────────────────────────────────────────

// Forward-declared here so that Value::operator[] can call it; defined below
// after the Value class closes.
[[noreturn]] void throwJSException(JSContext* ctx);

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
    class PropertyRef {
        Value* owner_{nullptr};
        std::string name_;
        bool is_index_{false};
        uint32_t index_{0};

        template <typename F, typename = void>
        struct has_call_operator_ : std::false_type {};

        template <typename F>
        struct has_call_operator_<F, std::void_t<decltype(&F::operator())>>
            : std::true_type {};

        template <typename F>
        static constexpr bool is_callable_like_v =
            std::is_function_v<std::remove_pointer_t<std::decay_t<F>>> ||
            std::is_member_function_pointer_v<std::decay_t<F>> ||
            has_call_operator_<std::decay_t<F>>::value;

        bool set_(Value v) {
            return is_index_ ? owner_->set(index_, std::move(v))
                             : owner_->set(name_, std::move(v));
        }

    public:
        PropertyRef(Value* owner, const char* name)
            : owner_(owner), name_(name ? name : "") {}
        PropertyRef(Value* owner, std::string name)
            : owner_(owner), name_(std::move(name)) {}
        PropertyRef(Value* owner, uint32_t index)
            : owner_(owner), is_index_(true), index_(index) {}

        operator Value() const {
            const Value* cowner = owner_;
            return is_index_ ? (*cowner)[index_] : (*cowner)[name_];
        }

        PropertyRef& operator=(Value v) {
            set_(std::move(v));
            return *this;
        }

        PropertyRef& operator=(JSValueConst v) {
            if (is_index_) {
                owner_->set(index_, v);
            } else {
                owner_->set(name_, v);
            }
            return *this;
        }

        template <typename Fn,
                  std::enable_if_t<
                      is_callable_like_v<Fn> &&
                      !std::is_convertible_v<std::decay_t<Fn>, Value> &&
                      !std::is_same_v<std::decay_t<Fn>, JSValue> &&
                      !std::is_same_v<std::decay_t<Fn>, JSValueConst>,
                      int> = 0>
        PropertyRef& operator=(Fn&& fn) {
            if (is_index_) {
                owner_->setFunction(index_, std::forward<Fn>(fn));
            } else {
                owner_->setFunction(name_, std::forward<Fn>(fn));
            }
            return *this;
        }

        template <typename U,
                  std::enable_if_t<
                      !is_callable_like_v<U> &&
                      !std::is_convertible_v<std::decay_t<U>, Value> &&
                      !std::is_same_v<std::decay_t<U>, JSValue> &&
                      !std::is_same_v<std::decay_t<U>, JSValueConst>,
                      int> = 0>
        PropertyRef& operator=(U&& v) {
            auto* ctx = owner_->ctx();
            using V = std::decay_t<U>;
            Value jsv(ctx, Converter<V, void>::to_js(ctx, std::forward<U>(v)));
            set_(std::move(jsv));
            return *this;
        }
    };

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
    PropertyRef operator[](const char* name) {
        return PropertyRef(this, name);
    }
    Value operator[](const char* name) const {
        JSValue v = JS_GetPropertyStr(ctx_, val_, name);
        if (JS_IsException(v)) { JS_FreeValue(ctx_, v); throwJSException(ctx_); }
        return Value(ctx_, v);
    }
    PropertyRef operator[](const std::string& name) {
        return PropertyRef(this, name);
    }
    Value operator[](const std::string& name) const { return (*this)[name.c_str()]; }
    PropertyRef operator[](uint32_t idx) {
        return PropertyRef(this, idx);
    }
    Value operator[](uint32_t idx) const {
        JSValue v = JS_GetPropertyUint32(ctx_, val_, idx);
        if (JS_IsException(v)) { JS_FreeValue(ctx_, v); throwJSException(ctx_); }
        return Value(ctx_, v);
    }

    bool set(const char* name, Value v) {
        return JS_SetPropertyStr(ctx_, val_, name, v.steal()) >= 0;
    }
    bool set(const std::string& name, Value v) { return set(name.c_str(), std::move(v)); }
    bool set(const char* name, JSValueConst v) {
        return JS_SetPropertyStr(ctx_, val_, name, JS_DupValue(ctx_, v)) >= 0;
    }
    bool set(const std::string& name, JSValueConst v) { return set(name.c_str(), v); }
    bool set(uint32_t idx, Value v) {
        return JS_SetPropertyUint32(ctx_, val_, idx, v.steal()) >= 0;
    }
    bool set(uint32_t idx, JSValueConst v) {
        return JS_SetPropertyUint32(ctx_, val_, idx, JS_DupValue(ctx_, v)) >= 0;
    }

    template <typename Fn>
    bool setFunction(const char* name, Fn&& fn, int length = -1);
    template <typename Fn>
    bool setFunction(const std::string& name, Fn&& fn, int length = -1) {
        return setFunction(name.c_str(), std::forward<Fn>(fn), length);
    }
    template <typename Fn>
    bool setFunction(uint32_t idx, Fn&& fn, int length = -1);

    // ── Array length ──────────────────────────────────────────────────────────
    int64_t length() const {
        Value lv(ctx_, JS_GetPropertyStr(ctx_, val_, "length"));
        int64_t n = 0;
        JS_ToInt64(ctx_, &n, lv.get());
        return n;
    }

    // ── Calling (low-level: raw JSValue array) ────────────────────────────────
    Value call(JSValueConst this_val, int argc = 0, JSValue* argv = nullptr) const {
        return Value(ctx_, JS_Call(ctx_, val_, this_val, argc, argv));
    }
    Value call(int argc = 0, JSValue* argv = nullptr) const {
        return call(JS_UNDEFINED, argc, argv);
    }
    Value callAsConstructor(int argc = 0, JSValue* argv = nullptr) const {
        return Value(ctx_, JS_CallConstructor(ctx_, val_, argc, argv));
    }

    // ── Calling (high-level: auto-convert C++ args / return value) ────────────
    /// Call this value as a function, automatically converting each C++ argument
    /// via Converter<T>::to_js.  The return type defaults to Value; supply an
    /// explicit template argument (e.g. invoke<int>(...)) to receive an already-
    /// converted C++ value.  Pass Ret = void to discard the result.
    /// Throws JSException if the JS call results in an exception.
    template <typename Ret = Value, typename... Args>
    Ret invoke(Args&&... args) const;

    /// Same as `invoke`, but lets you supply an explicit `this` object.
    template <typename Ret = Value, typename... Args>
    Ret invokeMethod(JSValueConst this_val, Args&&... args) const;

    /// Call this value as a constructor (`new`), automatically converting C++ args.
    template <typename Ret = Value, typename... Args>
    Ret invokeAsConstructor(Args&&... args) const;
};

// ── Pending-exception helpers ─────────────────────────────────────────────────

/// Fetch and throw the pending JS exception as a C++ JSException.
/// `JSException::what()` returns the JS error message string;
/// `JSException::stack()` returns the optional stack trace.
[[noreturn]] inline void throwJSException(JSContext* ctx) {
    JSValue exc = JS_GetException(ctx);

    std::string msg;
    JSValue ms = JS_ToString(ctx, exc);
    if (!JS_IsException(ms)) {
        size_t len;
        const char* p = JS_ToCStringLen(ctx, &len, ms);
        if (p) { msg.assign(p, len); JS_FreeCString(ctx, p); }
    }
    JS_FreeValue(ctx, ms);

    std::string stack;
    JSValue sv = JS_GetPropertyStr(ctx, exc, "stack");
    if (!JS_IsException(sv) && !JS_IsUndefined(sv)) {
        size_t len;
        const char* p = JS_ToCStringLen(ctx, &len, sv);
        if (p) { stack.assign(p, len); JS_FreeCString(ctx, p); }
    }
    JS_FreeValue(ctx, sv);
    JS_FreeValue(ctx, exc);

    throw JSException(msg, std::move(stack));
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
    ModuleLoader moduleLoader;

    explicit Context(Runtime& rt)
        : ctx_(JS_NewContext(rt.get())),
          moduleLoader(detail::default_module_loader_()) {
        if (!ctx_) throw Exception("Failed to create JSContext");
        JS_SetContextOpaque(ctx_, this);
    }
    explicit Context(JSRuntime* rt) : ctx_(JS_NewContext(rt)) {
        if (!ctx_) throw Exception("Failed to create JSContext");
        JS_SetContextOpaque(ctx_, this);
        moduleLoader = detail::default_module_loader_();
    }
    ~Context() noexcept {
        if (ctx_) {
            JS_SetContextOpaque(ctx_, nullptr);
            JS_FreeContext(ctx_);
        }
    }

    Context(const Context&)            = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&& o) noexcept
        : ctx_(o.ctx_), moduleLoader(std::move(o.moduleLoader)) {
        o.ctx_ = nullptr;
        if (ctx_) {
            JS_SetContextOpaque(ctx_, this);
        }
    }

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
    template <typename Fn,
              std::enable_if_t<!std::is_convertible_v<Fn, RawFunctionCallback>, int> = 0>
    void bindFunction(const std::string& name, Fn&& fn, int length = -1);
    template <typename RawFn,
              std::enable_if_t<std::is_convertible_v<RawFn, RawFunctionCallback>, int> = 0>
    void bindFunction(const std::string& name, RawFn&& fn, int length = -1);
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

    template <typename Fn,
              std::enable_if_t<!std::is_convertible_v<Fn, RawFunctionCallback>, int> = 0>
    Module& bindFunction(const std::string& name, Fn&& fn, int length = -1);
    template <typename RawFn,
              std::enable_if_t<std::is_convertible_v<RawFn, RawFunctionCallback>, int> = 0>
    Module& bindFunction(const std::string& name, RawFn&& fn, int length = -1);
    Module& bindFunctionRaw(const std::string& name,
                            RawFunctionCallback fn,
                            int length = -1);

    template <typename T>
    ClassDef<T> beginClass(const char* name);

    template <typename T, typename Base>
    ClassDef<T> deriveClass(const char* name);
};

inline Module Context::newModule(const std::string& name) {
    return Module(ctx_, name);
}

inline JSModuleDef* detail::module_loader_dispatch_(JSContext* ctx,
                                                    const char* module_name,
                                                    void* opaque) {
    auto* context = static_cast<Context*>(JS_GetContextOpaque(ctx));
    (void)opaque;
    if (!module_name)
        return nullptr;

    ModuleData data;
    try {
        if (context && context->moduleLoader) {
            data = context->moduleLoader(module_name);
        } else {
            data = detail::default_module_loader_()(module_name);
        }
    } catch (const std::exception& e) {
        JS_ThrowInternalError(ctx, "%s", e.what());
        return nullptr;
    } catch (...) {
        JS_ThrowInternalError(ctx, "Unknown error");
        return nullptr;
    }

    if (!data.source) {
        JS_ThrowReferenceError(ctx,
                               "could not load module filename '%s'",
                               module_name);
        return nullptr;
    }
    if (!data.url) {
        data.url = std::string(module_name);
    }

    JSValue func_val = JS_Eval(ctx,
                               data.source->c_str(),
                               data.source->size(),
                               module_name,
                               JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(func_val))
        return nullptr;
    if (JS_VALUE_GET_TAG(func_val) != JS_TAG_MODULE) {
        JS_FreeValue(ctx, func_val);
        JS_ThrowTypeError(ctx, "Loaded script is not a module: '%s'", module_name);
        return nullptr;
    }

    auto* module = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(func_val));
    JSValue meta = JS_GetImportMeta(ctx, module);
    if (JS_IsException(meta))
        return nullptr;

    if (JS_SetPropertyStr(ctx,
                          meta,
                          "url",
                          JS_NewStringLen(ctx, data.url->c_str(), data.url->size())) < 0) {
        JS_FreeValue(ctx, meta);
        return nullptr;
    }
    if (JS_SetPropertyStr(ctx, meta, "main", JS_NewBool(ctx, false)) < 0) {
        JS_FreeValue(ctx, meta);
        return nullptr;
    }
    JS_FreeValue(ctx, meta);
    return module;
}

} // namespace qjsb
