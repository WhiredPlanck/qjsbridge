// SPDX-License-Identifier: Apache-2.0
// qjsbridge – C++ class / struct binding
#pragma once

#include "core.hpp"
#include "convert.hpp"
#include "function.hpp"
#include "detail/meta.hpp"

#include <memory>
#include <string>
#include <type_traits>

namespace qjsb {
namespace detail {

// ── Object-holder hierarchy ───────────────────────────────────────────────────
// Stored as opaque data inside every bound-class JS object.
// The finaliser deletes the holder, which in turn may delete the wrapped T*.

template <typename T>
struct HolderBase {
    virtual ~HolderBase()                        = default;
    virtual T*              get()                = 0;
    virtual const T*        get() const          = 0;
    virtual std::shared_ptr<T> get_shared() const { return {}; }
};

template <typename T>
struct OwnedHolder final : HolderBase<T> {
    T* ptr;
    explicit OwnedHolder(T* p) : ptr(p) {}
    ~OwnedHolder() override { delete ptr; }
    T*       get()       override { return ptr; }
    const T* get() const override { return ptr; }
};

template <typename T>
struct BorrowedHolder final : HolderBase<T> {
    T* ptr;
    explicit BorrowedHolder(T* p) : ptr(p) {}
    // No destructor: we do not own the object.
    T*       get()       override { return ptr; }
    const T* get() const override { return ptr; }
};

template <typename T>
struct SharedPtrHolder final : HolderBase<T> {
    std::shared_ptr<T> ptr;
    explicit SharedPtrHolder(std::shared_ptr<T> p) : ptr(std::move(p)) {}
    T*       get()       override { return ptr.get(); }
    const T* get() const override { return ptr.get(); }
    std::shared_ptr<T> get_shared() const override { return ptr; }
};

// ── ClassRegistry<T> ─────────────────────────────────────────────────────────
// Holds the global JSClassID for T.
// Per-runtime registration is handled by calling JS_NewClass on every binding
// operation; JS_NewClass is a no-op (returns -1) if the class is already
// registered with that runtime, so it is safe to call repeatedly.

template <typename T>
struct ClassRegistry {
    static JSClassID& class_id() {
        static JSClassID id = 0;
        return id;
    }

    static const char*& class_name() {
        static const char* n = "unknown";
        return n;
    }

    // JSClassDef finaliser: delete the holder (which may delete T*).
    static void finalizer(JSRuntime* /*rt*/, JSValue val) {
        auto* h = static_cast<HolderBase<T>*>(JS_GetOpaque(val, class_id()));
        delete h;
    }

    static void ensure_registered(JSRuntime* rt, const char* name = nullptr) {
        if (name) class_name() = name;
        // JS_NewClassID is idempotent (no-op when id != 0).
        // JS_NewClass returns -1 if already registered – safe to ignore.
        qjsb_new_class_id(rt, &class_id());
        JSClassDef def{};
        def.class_name = class_name();
        def.finalizer  = finalizer;
        JS_NewClass(rt, class_id(), &def);
    }

    // ── Object extraction ─────────────────────────────────────────────────────
    static T* get_ptr(JSContext* ctx, JSValueConst val) {
        auto* h = static_cast<HolderBase<T>*>(
            JS_GetOpaque2(ctx, val, class_id()));
        return h ? h->get() : nullptr;
    }

    static std::shared_ptr<T> get_shared(JSContext* ctx, JSValueConst val) {
        auto* h = static_cast<HolderBase<T>*>(
            JS_GetOpaque2(ctx, val, class_id()));
        return h ? h->get_shared() : std::shared_ptr<T>{};
    }

    // ── Object wrapping ───────────────────────────────────────────────────────
    static JSValue push_owned(JSContext* ctx, T* ptr) {
        JSValue proto = JS_GetClassProto(ctx, class_id());
        JSValue obj   = JS_NewObjectProtoClass(ctx, proto, class_id());
        JS_FreeValue(ctx, proto);
        JS_SetOpaque(obj, static_cast<HolderBase<T>*>(new OwnedHolder<T>(ptr)));
        return obj;
    }

    static JSValue push_borrowed(JSContext* ctx, T* ptr) {
        JSValue proto = JS_GetClassProto(ctx, class_id());
        JSValue obj   = JS_NewObjectProtoClass(ctx, proto, class_id());
        JS_FreeValue(ctx, proto);
        JS_SetOpaque(obj, static_cast<HolderBase<T>*>(new BorrowedHolder<T>(ptr)));
        return obj;
    }

    static JSValue push_shared(JSContext* ctx, std::shared_ptr<T> ptr) {
        JSValue proto = JS_GetClassProto(ctx, class_id());
        JSValue obj   = JS_NewObjectProtoClass(ctx, proto, class_id());
        JS_FreeValue(ctx, proto);
        JS_SetOpaque(obj, static_cast<HolderBase<T>*>(
            new SharedPtrHolder<T>(std::move(ptr))));
        return obj;
    }
};

// ── MethodCallable<T, Fn> ─────────────────────────────────────────────────────
// Wraps either:
//   • A member-function pointer  R(T::*)(Args...)
//   • A free function / lambda   R(T*, Args...)  (T* is the first parameter)
// In both cases 'this' is extracted from JS's this_val, not from argv.

template <typename T, typename Fn>
struct MethodCallable final : CallableBase {
    // Decay Fn: raw function types become function pointers.
    using FnStored  = std::decay_t<Fn>;
    FnStored fn_;

    using Traits    = function_traits<FnStored>;
    using Ret       = typename Traits::return_type;
    using ArgsTuple = typename Traits::args_tuple;
    static constexpr std::size_t Arity = Traits::arity;
    static constexpr bool is_member    = std::is_member_function_pointer_v<FnStored>;

    template <typename F>
    explicit MethodCallable(F&& fn) : fn_(std::forward<F>(fn)) {}

    JSValue call(JSContext* ctx,
                 JSValueConst this_val,
                 int argc,
                 JSValueConst* argv) override {
        T* self = ClassRegistry<T>::get_ptr(ctx, this_val);
        if (!self)
            return JS_ThrowTypeError(ctx, "Invalid 'this': expected bound C++ object");
        return with_cpp_exception_guard(ctx, [&]() {
            return invoke(ctx, self, argc, argv,
                          std::make_index_sequence<Arity>{});
        });
    }

private:
    // ── Member function invocation ────────────────────────────────────────────
    template <std::size_t... Is>
    JSValue invoke(JSContext* ctx, T* self,
                   int argc, JSValueConst* argv,
                   std::index_sequence<Is...>) {
        if constexpr (is_member) {
            // (self->*fn)(argv[0], argv[1], ...)
            if constexpr (std::is_void_v<Ret>) {
                (self->*fn_)(
                    extract_arg<std::tuple_element_t<Is, ArgsTuple>>(ctx, argc, argv, Is)...);
                return JS_UNDEFINED;
            } else {
                return Converter<std::decay_t<Ret>>::to_js(ctx,
                    (self->*fn_)(
                        extract_arg<std::tuple_element_t<Is, ArgsTuple>>(ctx, argc, argv, Is)...));
            }
        } else {
            // fn(self, argv[0], argv[1], ...)  – arg[0] is T*, rest from argv
            return invoke_free(ctx, self, argc, argv,
                               std::make_index_sequence<Arity>{});
        }
    }

    // For free-function style: arg index 0 → self, rest → argv[i-1]
    template <std::size_t... Is>
    JSValue invoke_free(JSContext* ctx, T* self,
                        int argc, JSValueConst* argv,
                        std::index_sequence<Is...>) {
        if constexpr (std::is_void_v<Ret>) {
            fn_(extract_self_or_arg<Is>(ctx, self, argc, argv)...);
            return JS_UNDEFINED;
        } else {
            return Converter<std::decay_t<Ret>>::to_js(ctx,
                fn_(extract_self_or_arg<Is>(ctx, self, argc, argv)...));
        }
    }

    template <std::size_t I>
    auto extract_self_or_arg(JSContext* ctx, T* self,
                              int argc, JSValueConst* argv) {
        if constexpr (I == 0) {
            (void)ctx; (void)argc; (void)argv;
            return self;
        } else {
            return extract_arg<std::tuple_element_t<I, ArgsTuple>>(
                ctx, argc, argv, I - 1);
        }
    }
};

template <typename T>
struct RawMethodCallable final : CallableBase {
    using Fn = std::function<JSValue(JSContext*, T*, int, JSValueConst*)>;
    Fn fn_;

    explicit RawMethodCallable(Fn fn) : fn_(std::move(fn)) {}

    JSValue call(JSContext* ctx,
                 JSValueConst this_val,
                 int argc,
                 JSValueConst* argv) override {
        T* self = ClassRegistry<T>::get_ptr(ctx, this_val);
        if (!self)
            return JS_ThrowTypeError(ctx, "Invalid 'this': expected bound C++ object");
        return fn_(ctx, self, argc, argv);
    }
};

// ── ConstructorCallable<T, Args...> ──────────────────────────────────────────

template <typename T, typename... Args>
struct ConstructorCallable final : CallableBase {
    JSValue call(JSContext* ctx,
                 JSValueConst /*this_val*/,
                 int argc,
                 JSValueConst* argv) override {
        return with_cpp_exception_guard(ctx, [&]() {
            return call_impl(ctx, argc, argv,
                             std::make_index_sequence<sizeof...(Args)>{});
        });
    }

    template <std::size_t... Is>
    JSValue call_impl(JSContext* ctx, int argc, JSValueConst* argv,
                      std::index_sequence<Is...>) {
        T* obj = new T(extract_arg<Args>(ctx, argc, argv, Is)...);
        return ClassRegistry<T>::push_owned(ctx, obj);
    }
};

// ── FieldGetter / FieldSetter ─────────────────────────────────────────────────

template <typename T, typename M>
struct FieldGetter final : CallableBase {
    M T::* member_;
    explicit FieldGetter(M T::* m) : member_(m) {}

    JSValue call(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) override {
        T* self = ClassRegistry<T>::get_ptr(ctx, this_val);
        if (!self) return JS_ThrowTypeError(ctx, "Invalid 'this'");
        return with_cpp_exception_guard(ctx, [&]() {
            return Converter<remove_cvref_t<M>>::to_js(ctx, self->*member_);
        });
    }
};

template <typename T, typename M>
struct FieldSetter final : CallableBase {
    M T::* member_;
    explicit FieldSetter(M T::* m) : member_(m) {}

    JSValue call(JSContext* ctx, JSValueConst this_val,
                 int argc, JSValueConst* argv) override {
        T* self = ClassRegistry<T>::get_ptr(ctx, this_val);
        if (!self) return JS_ThrowTypeError(ctx, "Invalid 'this'");
        if (argc < 1) return JS_ThrowTypeError(ctx, "Setter requires 1 argument");
        return with_cpp_exception_guard(ctx, [&]() -> JSValue {
            self->*member_ = Converter<remove_cvref_t<M>>::from_js(ctx, argv[0]);
            return JS_UNDEFINED;
        });
    }
};

} // namespace detail

// ── ClassDef<T>: fluent class definition API ──────────────────────────────────
//
//   ClassDef<MyClass>(ctx, "MyClass")
//       .constructor<int, std::string>()
//       .method("getName", &MyClass::getName)
//       .method("setName", &MyClass::setName)
//       .field("value", &MyClass::value)          // direct member access
//       .staticMethod("create", &MyClass::create)
//       .endClass();
//

template <typename T>
class ClassDef {
    JSContext*  ctx_;
    Module*     module_{nullptr};
    std::string name_;
    JSValue     proto_;   // prototype object (owned by this ClassDef)
    JSValue     ctor_;    // constructor function (JS_UNDEFINED until .constructor<>())

    // Wrap a CallableBase* as a JS function.
    JSValue make_fn_(detail::CallableBase* cb, int arity) {
        return detail::wrap_callable(ctx_, cb, arity);
    }

public:
    using RawMethodCallback = std::function<JSValue(JSContext*, T*, int, JSValueConst*)>;
    ClassDef(JSContext* ctx,
             const char* name,
             JSValueConst base_proto = JS_UNDEFINED)
        : ctx_(ctx), name_(name),
          proto_(JS_NewObject(ctx)),
          ctor_(JS_UNDEFINED) {
        detail::ClassRegistry<T>::ensure_registered(JS_GetRuntime(ctx_), name);
        if (!JS_IsUndefined(base_proto) && !JS_IsNull(base_proto)) {
            JS_SetPrototype(ctx_, proto_, base_proto);
        }
        // Publish the prototype so that objects created with push_owned/etc.
        // pick it up via JS_GetClassProto.
        JS_SetClassProto(ctx_, detail::ClassRegistry<T>::class_id(),
                         JS_DupValue(ctx_, proto_));
    }

    explicit ClassDef(Context& ctx, const char* name)
        : ClassDef(ctx.get(), name) {}

    explicit ClassDef(Module& module,
                      const char* name,
                      JSValueConst base_proto = JS_UNDEFINED)
        : ClassDef(module.context(), name, base_proto) {
        module_ = &module;
    }

    ~ClassDef() noexcept {
        JS_FreeValue(ctx_, proto_);
        JS_FreeValue(ctx_, ctor_);
    }

    ClassDef(const ClassDef&)            = delete;
    ClassDef& operator=(const ClassDef&) = delete;

    // ── Constructor ───────────────────────────────────────────────────────────

    template <typename... Args>
    ClassDef& constructor() {
        auto* cb = new detail::ConstructorCallable<T, Args...>();

        detail::ensure_callable_class_(JS_GetRuntime(ctx_));
        JSValue wrapper = JS_NewObjectClass(ctx_,
            static_cast<int>(detail::callable_class_id_()));
        JS_SetOpaque(wrapper, static_cast<detail::CallableBase*>(cb));

        JSValue func = JS_NewCFunctionData(ctx_, ctor_dispatch_,
                                           static_cast<int>(sizeof...(Args)),
                                           0, 1, &wrapper);
        JS_FreeValue(ctx_, wrapper);

        // Mark as a constructor so JS `new` works.
        JS_SetConstructorBit(ctx_, func, true);

        // Constructor.prototype → proto_, proto_.constructor → Constructor
        JS_SetPropertyStr(ctx_, func, "prototype", JS_DupValue(ctx_, proto_));
        JS_SetPropertyStr(ctx_, proto_, "constructor", JS_DupValue(ctx_, func));

        JS_FreeValue(ctx_, ctor_);
        ctor_ = func;
        return *this;
    }

    // ── Instance methods ──────────────────────────────────────────────────────

    // Member function pointer (const or non-const).
    template <typename MFn,
              std::enable_if_t<std::is_member_function_pointer_v<MFn>, int> = 0>
    ClassDef& method(const char* name, MFn fn) {
        using Traits = detail::function_traits<MFn>;
        auto* cb = new detail::MethodCallable<T, MFn>(fn);
        JS_SetPropertyStr(ctx_, proto_, name,
                          make_fn_(cb, static_cast<int>(Traits::arity)));
        return *this;
    }

    // Free function / lambda whose first parameter is T* (or const T*).
    template <typename Fn,
              std::enable_if_t<
                  !std::is_member_function_pointer_v<std::decay_t<Fn>> &&
                  !std::is_convertible_v<Fn, RawMethodCallback>,
                  int> = 0>
    ClassDef& method(const char* name, Fn&& fn) {
        using Traits = detail::function_traits<std::decay_t<Fn>>;
        // JS arity = C++ arity − 1 (T* self is implicit in JS).
        int js_arity = static_cast<int>(Traits::arity) > 0
            ? static_cast<int>(Traits::arity) - 1 : 0;
        auto* cb = new detail::MethodCallable<T, std::decay_t<Fn>>(
            std::forward<Fn>(fn));
        JS_SetPropertyStr(ctx_, proto_, name, make_fn_(cb, js_arity));
        return *this;
    }

    /// Low-level method callback:
    ///   fn(ctx, self, argc, argv) -> JSValue
    /// Useful for manual overload dispatch by checking argc/argument types.
    /// Callback must be copyable (stored as std::function).
    template <typename Fn,
              std::enable_if_t<std::is_convertible_v<Fn, RawMethodCallback>, int> = 0>
    ClassDef& method(const char* name, Fn&& fn, int length = -1) {
        int js_arity = (length >= 0) ? length : 0;
        auto* cb = new detail::RawMethodCallable<T>(RawMethodCallback(std::forward<Fn>(fn)));
        JS_SetPropertyStr(ctx_, proto_, name, make_fn_(cb, js_arity));
        return *this;
    }

    /// Low-level method callback:
    ///   fn(ctx, self, argc, argv) -> JSValue
    /// Useful for manual overload dispatch by checking argc/argument types.
    ClassDef& rawMethod(const char* name,
                        RawMethodCallback fn,
                        int length = -1) {
        return method(name, std::move(fn), length);
    }

    // ── Data fields (auto getter+setter via JS accessors) ─────────────────────

    template <typename M>
    ClassDef& field(const char* name, M T::* member) {
        auto* getter = new detail::FieldGetter<T, M>(member);
        auto* setter = new detail::FieldSetter<T, M>(member);
        JSValue gf = make_fn_(getter, 0);
        JSValue sf = make_fn_(setter, 1);
        JSAtom atom = JS_NewAtom(ctx_, name);
        JS_DefinePropertyGetSet(ctx_, proto_, atom, gf, sf,
                                JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx_, atom);
        return *this;
    }

    // ── Property overloads ─────────────────────────────────────────────────────
    // property(name, getter)                    -> read-only
    // property(name, getter, setter)            -> read-write

    template <typename Getter>
    ClassDef& property(const char* name, Getter&& getter) {
        JSValue gf = make_getter_fn_(std::forward<Getter>(getter));
        JSAtom  at = JS_NewAtom(ctx_, name);
        JS_DefinePropertyGetSet(ctx_, proto_, at, gf, JS_UNDEFINED,
                                JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx_, at);
        return *this;
    }

    template <typename Getter, typename Setter>
    ClassDef& property(const char* name, Getter&& getter, Setter&& setter) {
        JSValue gf = make_getter_fn_(std::forward<Getter>(getter));
        JSValue sf = make_setter_fn_(std::forward<Setter>(setter));
        JSAtom  at = JS_NewAtom(ctx_, name);
        JS_DefinePropertyGetSet(ctx_, proto_, at, gf, sf,
                                JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx_, at);
        return *this;
    }

    // Backward-compatible alias.
    template <typename Getter>
    ClassDef& readonlyProp(const char* name, Getter&& getter) {
        return property(name, std::forward<Getter>(getter));
    }

    // ── Static methods ────────────────────────────────────────────────────────

    template <typename Fn>
    ClassDef& staticMethod(const char* name, Fn&& fn) {
        if (JS_IsUndefined(ctor_))
            throw Exception("staticMethod: define a constructor first");
        JSValue f = detail::make_js_function_(ctx_, std::forward<Fn>(fn), name);
        JS_SetPropertyStr(ctx_, ctor_, name, f);
        return *this;
    }

    // ── Finalise and register in global/module scope ──────────────────────────

    /// Installs the constructor as a global variable.
    ClassDef& endClassGlobal(const std::string& global_name = "") {
        ensure_ctor_();
        JSValue g = JS_GetGlobalObject(ctx_);
        const std::string& gn = global_name.empty() ? name_ : global_name;
        JS_SetPropertyStr(ctx_, g, gn.c_str(), JS_DupValue(ctx_, ctor_));
        JS_FreeValue(ctx_, g);
        return *this;
    }

    /// Exports the constructor on the module this class was started from.
    /// Returns the module to support chained `beginClass` calls.
    Module& endClass(const std::string& export_name = "") {
        if (!module_)
            throw Exception("endClass: class is not attached to a module");
        ensure_ctor_();
        const std::string& en = export_name.empty() ? name_ : export_name;
        module_->exportValue(en, Value::dup(ctx_, ctor_));
        return *module_;
    }

    /// Exports the constructor from a module so it can be imported from JS.
    ClassDef& endClass(Module& module, const std::string& export_name = "") {
        ensure_ctor_();
        const std::string& en = export_name.empty() ? name_ : export_name;
        module.exportValue(en, Value::dup(ctx_, ctor_));
        return *this;
    }

    /// Returns the constructor as a Value (for manual placement).
    Value constructorValue() const { return Value::dup(ctx_, ctor_); }

private:
    void ensure_ctor_() {
        if (!JS_IsUndefined(ctor_))
            return;
        // No constructor defined: create a no-op constructor stub that
        // produces a TypeError at `new` time.
        detail::ensure_callable_class_(JS_GetRuntime(ctx_));

        struct NoCtorCallable final : detail::CallableBase {
            std::string name;
            explicit NoCtorCallable(std::string n) : name(std::move(n)) {}
            JSValue call(JSContext* ctx, JSValueConst, int, JSValueConst*) override {
                return JS_ThrowTypeError(ctx,
                    "No constructor registered for class '%s'", name.c_str());
            }
        };
        auto* cb = static_cast<detail::CallableBase*>(
            new NoCtorCallable(name_));
        JSValue wrapper = JS_NewObjectClass(ctx_,
            static_cast<int>(detail::callable_class_id_()));
        JS_SetOpaque(wrapper, cb);
        ctor_ = JS_NewCFunctionData(ctx_, ctor_dispatch_, 0, 0, 1, &wrapper);
        JS_FreeValue(ctx_, wrapper);
        JS_SetConstructorBit(ctx_, ctor_, true);
        JS_SetPropertyStr(ctx_, ctor_, "prototype", JS_DupValue(ctx_, proto_));
        JS_SetPropertyStr(ctx_, proto_, "constructor", JS_DupValue(ctx_, ctor_));
    }

    // Constructor dispatcher (same pattern as callable_dispatch_ but acts as ctor).
    static JSValue ctor_dispatch_(JSContext* ctx,
                                   JSValueConst this_val,
                                   int argc,
                                   JSValueConst* argv,
                                   int /*magic*/,
                                   JSValue* func_data) {
        auto* cb = static_cast<detail::CallableBase*>(
            JS_GetOpaque(func_data[0], detail::callable_class_id_()));
        if (!cb) return JS_ThrowTypeError(ctx, "Invalid constructor callable");
        return detail::with_cpp_exception_guard(ctx, [&]() {
            return cb->call(ctx, this_val, argc, argv);
        });
    }

    // Helper for property(): wrap any getter callable into a JS function.
    template <typename G>
    JSValue make_getter_fn_(G&& g) {
        auto* cb = new detail::MethodCallable<T, std::decay_t<G>>(std::forward<G>(g));
        return make_fn_(cb, 0);
    }

    // Helper for property(): wrap any setter callable into a JS function.
    template <typename S>
    JSValue make_setter_fn_(S&& s) {
        auto* cb = new detail::MethodCallable<T, std::decay_t<S>>(std::forward<S>(s));
        return make_fn_(cb, 1);
    }
};

template <typename T>
inline ClassDef<T> Module::beginClass(const char* name) {
    return ClassDef<T>(*this, name);
}

template <typename T, typename Base>
inline ClassDef<T> Module::deriveClass(const char* name) {
    static_assert(std::is_base_of_v<Base, T>,
                  "deriveClass<T, Base>: Base must be a base class of T");
    detail::ClassRegistry<Base>::ensure_registered(JS_GetRuntime(ctx_));
    JSValue base_proto = JS_GetClassProto(ctx_, detail::ClassRegistry<Base>::class_id());
    if (!JS_IsObject(base_proto)) {
        JS_FreeValue(ctx_, base_proto);
        throw Exception("deriveClass: base class must be registered first");
    }
    ClassDef<T> def(*this, name, base_proto);
    JS_FreeValue(ctx_, base_proto);
    return def;
}

// ── Converter specialisations for bound C++ classes ──────────────────────────
//
// Value semantics (T by value): copies/moves T onto the heap, JS takes
// ownership.  This covers any user class that is:
//   • a class type
//   • not a known STL container / smart-pointer / utility type
//   • not qjsb::Value
//
// This specialisation is the fallback; the more-specific pointer/reference
// specialisations below take precedence for T* / const T* / shared_ptr<T> etc.
template <typename T>
struct Converter<T, std::enable_if_t<
    std::is_class_v<T> &&
    !detail::is_specialization_v<T, std::vector> &&
    !detail::is_specialization_v<T, std::map> &&
    !detail::is_specialization_v<T, std::unordered_map> &&
    !detail::is_specialization_v<T, std::set> &&
    !detail::is_specialization_v<T, std::unordered_set> &&
    !detail::is_specialization_v<T, std::optional> &&
    !detail::is_specialization_v<T, std::shared_ptr> &&
    !detail::is_specialization_v<T, std::basic_string> &&
    !detail::is_specialization_v<T, std::basic_string_view> &&
    !detail::is_std_array_v<T> &&
    !std::is_same_v<T, Value>
>> {
    static JSValue to_js(JSContext* ctx, T v) {
        return detail::ClassRegistry<T>::push_owned(ctx, new T(std::move(v)));
    }
    static T from_js(JSContext* ctx, JSValueConst v) {
        T* ptr = detail::ClassRegistry<T>::get_ptr(ctx, v);
        if (!ptr) throw TypeError("Expected bound C++ object");
        return *ptr;   // copy
    }
};

// T* → JS  (JS takes ownership and will call delete via the finaliser)
template <typename T>
struct Converter<T*, std::enable_if_t<std::is_class_v<T>>> {
    static JSValue to_js(JSContext* ctx, T* ptr) {
        if (!ptr) return JS_NULL;
        return detail::ClassRegistry<T>::push_owned(ctx, ptr);
    }
    static T* from_js(JSContext* ctx, JSValueConst v) {
        if (JS_IsNull(v) || JS_IsUndefined(v)) return nullptr;
        return detail::ClassRegistry<T>::get_ptr(ctx, v);
    }
};

// const T* → JS  (borrowed; JS will not delete)
template <typename T>
struct Converter<const T*, std::enable_if_t<std::is_class_v<T>>> {
    static JSValue to_js(JSContext* ctx, const T* ptr) {
        if (!ptr) return JS_NULL;
        return detail::ClassRegistry<T>::push_borrowed(ctx, const_cast<T*>(ptr));
    }
    static const T* from_js(JSContext* ctx, JSValueConst v) {
        if (JS_IsNull(v) || JS_IsUndefined(v)) return nullptr;
        return detail::ClassRegistry<T>::get_ptr(ctx, v);
    }
};

// std::shared_ptr<T> → JS
template <typename T>
struct Converter<std::shared_ptr<T>> {
    static JSValue to_js(JSContext* ctx, std::shared_ptr<T> ptr) {
        if (!ptr) return JS_NULL;
        return detail::ClassRegistry<T>::push_shared(ctx, std::move(ptr));
    }
    static std::shared_ptr<T> from_js(JSContext* ctx, JSValueConst v) {
        if (JS_IsNull(v) || JS_IsUndefined(v)) return {};
        return detail::ClassRegistry<T>::get_shared(ctx, v);
    }
};

// T& → (extract only; T& cannot be round-tripped through to_js safely)
template <typename T>
struct Converter<T&, std::enable_if_t<std::is_class_v<T>>> {
    static T& from_js(JSContext* ctx, JSValueConst v) {
        T* ptr = detail::ClassRegistry<T>::get_ptr(ctx, v);
        if (!ptr) throw TypeError("Expected bound C++ object for T&");
        return *ptr;
    }
};

// const T& → JS (borrowed) / from JS
template <typename T>
struct Converter<const T&,
    std::enable_if_t<
        std::is_class_v<T> &&
        !std::is_same_v<T, std::string> &&
        !std::is_same_v<T, Value>>> {

    static JSValue to_js(JSContext* ctx, const T& ref) {
        return detail::ClassRegistry<T>::push_borrowed(ctx, const_cast<T*>(&ref));
    }
    static const T& from_js(JSContext* ctx, JSValueConst v) {
        const T* ptr = detail::ClassRegistry<T>::get_ptr(ctx, v);
        if (!ptr) throw TypeError("Expected bound C++ object for const T&");
        return *ptr;
    }
};

// ── Free helpers ──────────────────────────────────────────────────────────────

/// Push a C++ object to JS; JS takes ownership (will delete on GC).
template <typename T>
inline Value pushOwned(JSContext* ctx, T* ptr) {
    return Value(ctx, detail::ClassRegistry<T>::push_owned(ctx, ptr));
}
template <typename T>
inline Value pushOwned(Context& ctx, T* ptr) { return pushOwned(ctx.get(), ptr); }

/// Push a C++ object to JS; JS borrows it (will NOT delete).
template <typename T>
inline Value pushBorrowed(JSContext* ctx, T* ptr) {
    return Value(ctx, detail::ClassRegistry<T>::push_borrowed(ctx, ptr));
}
template <typename T>
inline Value pushBorrowed(Context& ctx, T* ptr) { return pushBorrowed(ctx.get(), ptr); }

/// Push a shared_ptr<T> to JS.
template <typename T>
inline Value pushShared(JSContext* ctx, std::shared_ptr<T> ptr) {
    return Value(ctx, detail::ClassRegistry<T>::push_shared(ctx, std::move(ptr)));
}
template <typename T>
inline Value pushShared(Context& ctx, std::shared_ptr<T> ptr) {
    return pushShared(ctx.get(), std::move(ptr));
}

/// Extract a T* from a JS-bound object (returns nullptr if not the right type).
template <typename T>
inline T* getPtr(JSContext* ctx, JSValueConst v) {
    return detail::ClassRegistry<T>::get_ptr(ctx, v);
}
template <typename T>
inline T* getPtr(Context& ctx, JSValueConst v) { return getPtr<T>(ctx.get(), v); }

} // namespace qjsb
