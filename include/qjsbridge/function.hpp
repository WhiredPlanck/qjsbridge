// SPDX-License-Identifier: MIT
// qjsbridge – function/lambda binding
#pragma once

#include "core.hpp"
#include "convert.hpp"
#include "detail/meta.hpp"

#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

namespace qjsb {
namespace detail {

// ── Type-erased callable ──────────────────────────────────────────────────────

struct CallableBase {
    virtual ~CallableBase() = default;
    virtual JSValue call(JSContext* ctx,
                         JSValueConst this_val,
                         int argc,
                         JSValueConst* argv) = 0;
};

struct RawCallable final : CallableBase {
    RawFunctionCallback fn_;
    explicit RawCallable(RawFunctionCallback fn) : fn_(std::move(fn)) {}

    JSValue call(JSContext* ctx,
                 JSValueConst this_val,
                 int argc,
                 JSValueConst* argv) override {
        return fn_(ctx, this_val, argc, argv);
    }
};

// ── Internal singleton: class ID for callable-wrapper JS objects ──────────────
// Stores a CallableBase* as opaque data in a plain JS object.

inline JSClassID& callable_class_id_() {
    static JSClassID id = 0;
    return id;
}

inline void callable_finalizer_(JSRuntime* /*rt*/, JSValue val) {
    auto* cb = static_cast<CallableBase*>(
        JS_GetOpaque(val, callable_class_id_()));
    delete cb;
}

inline void ensure_callable_class_(JSRuntime* rt) {
    // JS_NewClassID is idempotent (no-op when id != 0).
    // JS_NewClass returns -1 if already registered with this runtime – safe to ignore.
    qjsb_new_class_id(rt, &callable_class_id_());
    JSClassDef def{};
    def.class_name = "__qjsb_callable__";
    def.finalizer  = callable_finalizer_;
    JS_NewClass(rt, callable_class_id_(), &def);
}

// ── Static dispatcher invoked by JS_NewCFunctionData ─────────────────────────

inline JSValue callable_dispatch_(JSContext* ctx,
                                   JSValueConst this_val,
                                   int argc,
                                   JSValueConst* argv,
                                   int /*magic*/,
                                   JSValue* func_data) {
    auto* cb = static_cast<CallableBase*>(
        JS_GetOpaque(func_data[0], callable_class_id_()));
    if (!cb) return JS_ThrowTypeError(ctx, "Invalid callable wrapper");
    try {
        return cb->call(ctx, this_val, argc, argv);
    } catch (const std::exception& e) {
        JSValue err = JS_NewError(ctx);
        JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, e.what()));
        return JS_Throw(ctx, err);
    } catch (...) {
        return JS_ThrowInternalError(ctx, "Unknown C++ exception");
    }
}

// ── Argument extraction from JS argv ─────────────────────────────────────────

template <typename Arg>
inline remove_cvref_t<Arg> extract_arg(JSContext* ctx,
                                        int argc,
                                        JSValueConst* argv,
                                        std::size_t i) {
    using T = remove_cvref_t<Arg>;
    JSValueConst v = (i < static_cast<std::size_t>(argc)) ? argv[i] : JS_UNDEFINED;
    return Converter<T>::from_js(ctx, v);
}

// ── FreeCallable<Fn>: wraps free functions, lambdas, std::function ────────────

template <typename Fn>
struct FreeCallable final : CallableBase {
    // Decay Fn so that raw function types (e.g. int(int,int)) become pointers.
    using FnStored = std::decay_t<Fn>;
    FnStored fn_;

    using Traits    = function_traits<FnStored>;
    using Ret       = typename Traits::return_type;
    using ArgsTuple = typename Traits::args_tuple;
    static constexpr std::size_t Arity = Traits::arity;

    template <typename F>
    explicit FreeCallable(F&& fn) : fn_(std::forward<F>(fn)) {}

    JSValue call(JSContext* ctx,
                 JSValueConst /*this_val*/,
                 int argc,
                 JSValueConst* argv) override {
        return invoke(ctx, argc, argv, std::make_index_sequence<Arity>{});
    }

    template <std::size_t... Is>
    JSValue invoke(JSContext* ctx, int argc, JSValueConst* argv,
                   std::index_sequence<Is...>) {
        if constexpr (std::is_void_v<Ret>) {
            fn_(extract_arg<std::tuple_element_t<Is, ArgsTuple>>(ctx, argc, argv, Is)...);
            return JS_UNDEFINED;
        } else {
            return Converter<std::decay_t<Ret>>::to_js(
                ctx,
                fn_(extract_arg<std::tuple_element_t<Is, ArgsTuple>>(ctx, argc, argv, Is)...));
        }
    }
};

// ── Low-level helper: CallableBase* → JSValue (function) ─────────────────────

inline JSValue wrap_callable(JSContext* ctx,
                              CallableBase* cb,
                              int arity) {
    JSRuntime* rt = JS_GetRuntime(ctx);
    ensure_callable_class_(rt);

    JSValue wrapper = JS_NewObjectClass(ctx, static_cast<int>(callable_class_id_()));
    JS_SetOpaque(wrapper, cb);

    JSValue func = JS_NewCFunctionData(ctx, callable_dispatch_, arity, 0, 1, &wrapper);
    JS_FreeValue(ctx, wrapper);
    return func;
}

// ── make_js_function_: any callable → JSValue ────────────────────────────────

template <typename Fn>
inline JSValue make_js_function_(JSContext* ctx,
                                  Fn&& fn,
                                  const char* /*name*/ = "",
                                  int length = -1) {
    using FnType = std::decay_t<Fn>;
    int arity = (length >= 0)
        ? length
        : static_cast<int>(function_traits<FnType>::arity);

    auto* cb = static_cast<CallableBase*>(
        new FreeCallable<FnType>(std::forward<Fn>(fn)));
    return wrap_callable(ctx, cb, arity);
}

inline JSValue make_raw_js_function_(JSContext* ctx,
                                     RawFunctionCallback fn,
                                     int length = -1) {
    int arity = (length >= 0) ? length : 0;
    auto* cb = static_cast<CallableBase*>(
        new RawCallable(std::move(fn)));
    return wrap_callable(ctx, cb, arity);
}

} // namespace detail

// ── Public API ────────────────────────────────────────────────────────────────

/// Wrap any C++ callable (function pointer, lambda, std::function) as a
/// JS function Value.  'length' overrides the JS .length hint.
template <typename Fn>
inline Value makeFunction(JSContext* ctx,
                          Fn&& fn,
                          const char* name = "",
                          int length = -1) {
    return Value(ctx, detail::make_js_function_(ctx, std::forward<Fn>(fn), name, length));
}

template <typename Fn>
inline Value makeFunction(Context& ctx,
                          Fn&& fn,
                          const char* name = "",
                          int length = -1) {
    return makeFunction(ctx.get(), std::forward<Fn>(fn), name, length);
}

inline Value makeFunction(JSContext* ctx,
                          RawFunctionCallback fn,
                          const char* /*name*/ = "",
                          int length = -1) {
    return Value(ctx, detail::make_raw_js_function_(ctx, std::move(fn), length));
}

inline Value makeFunction(Context& ctx,
                          RawFunctionCallback fn,
                          const char* /*name*/ = "",
                          int length = -1) {
    return makeFunction(ctx.get(), std::move(fn), "", length);
}

// ── Context::bindFunction ─────────────────────────────────────────────────────

template <typename Fn>
inline void Context::bindFunction(const std::string& name, Fn&& fn, int length) {
    JSValue g    = JS_GetGlobalObject(ctx_);
    JSValue func = detail::make_js_function_(ctx_, std::forward<Fn>(fn),
                                              name.c_str(), length);
    JS_SetPropertyStr(ctx_, g, name.c_str(), func);
    JS_FreeValue(ctx_, g);
}

inline void Context::bindFunctionRaw(const std::string& name,
                                     RawFunctionCallback fn,
                                     int length) {
    JSValue g    = JS_GetGlobalObject(ctx_);
    JSValue func = detail::make_raw_js_function_(ctx_, std::move(fn), length);
    JS_SetPropertyStr(ctx_, g, name.c_str(), func);
    JS_FreeValue(ctx_, g);
}

template <typename Fn,
          std::enable_if_t<!std::is_convertible_v<Fn, RawFunctionCallback>, int>>
inline Module& Module::bindFunction(const std::string& name, Fn&& fn, int length) {
    if (!mod_) throw Exception("Invalid module");
    if (JS_AddModuleExport(ctx_, mod_, name.c_str()) < 0)
        throwJSException(ctx_);
    state_->exports.push_back(detail::ModuleExport{
        name,
        [fn = std::decay_t<Fn>(std::forward<Fn>(fn)), length](JSContext* ctx) mutable {
            return detail::make_js_function_(ctx, fn, "", length);
        }
    });
    return *this;
}

template <typename Fn,
          std::enable_if_t<std::is_convertible_v<Fn, RawFunctionCallback>, int>>
inline Module& Module::bindFunction(const std::string& name, Fn&& fn, int length) {
    if (!mod_) throw Exception("Invalid module");
    if (JS_AddModuleExport(ctx_, mod_, name.c_str()) < 0)
        throwJSException(ctx_);
    RawFunctionCallback raw_fn(std::forward<Fn>(fn));
    state_->exports.push_back(detail::ModuleExport{
        name,
        [raw_fn = std::move(raw_fn), length](JSContext* ctx) mutable {
            return detail::make_raw_js_function_(ctx, raw_fn, length);
        }
    });
    return *this;
}

inline Module& Module::bindFunctionRaw(const std::string& name,
                                       RawFunctionCallback fn,
                                       int length) {
    return bindFunction(name, std::move(fn), length);
}

} // namespace qjsb
