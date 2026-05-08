// SPDX-License-Identifier: Apache-2.0
// qjsbridge – type conversion system (Converter<T> specialisations)
#pragma once

#include "core.hpp"
#include "detail/meta.hpp"

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace qjsb {

// ── Converter<T> primary template ────────────────────────────────────────────
//
// Each specialisation must provide at least:
//   static JSValue to_js(JSContext*, T)         – produce an owned JSValue
//   static T       from_js(JSContext*, JSValueConst) – extract a C++ value
//
// Optionally:
//   static bool    accept(JSContext*, JSValueConst)  – type-check
//
// The to_js/from_js signatures accept T by value; callers can pass rvalues.

template <typename T, typename Enable = void>
struct Converter;

// ── Free helper functions ─────────────────────────────────────────────────────
// Use std::decay_t so that array types (e.g. char[N]) decay to pointers and
// function types decay to function pointers before the Converter lookup.

template <typename T>
inline JSValue to_js(JSContext* ctx, T&& val) {
    return Converter<std::decay_t<T>>::to_js(ctx, std::forward<T>(val));
}

template <typename T>
inline std::decay_t<T> from_js(JSContext* ctx, JSValueConst val) {
    return Converter<std::decay_t<T>>::from_js(ctx, val);
}

// ── void ─────────────────────────────────────────────────────────────────────

template <>
struct Converter<void> {
    static JSValue to_js(JSContext* /*ctx*/) { return JS_UNDEFINED; }
};

// ── bool ─────────────────────────────────────────────────────────────────────

template <>
struct Converter<bool> {
    static JSValue to_js(JSContext* ctx, bool v)      { return JS_NewBool(ctx, v ? 1 : 0); }
    static bool    from_js(JSContext* ctx, JSValueConst v) { return JS_ToBool(ctx, v) != 0; }
    static bool    accept(JSContext* /*ctx*/, JSValueConst v) { return JS_IsBool(v); }
};

// ── Integers (excluding bool) ─────────────────────────────────────────────────

template <typename T>
struct Converter<T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>>> {
    static JSValue to_js(JSContext* ctx, T v) {
        if constexpr (std::is_signed_v<T>) {
            if constexpr (sizeof(T) <= 4)
                return JS_NewInt32(ctx, static_cast<int32_t>(v));
            else
                return JS_NewInt64(ctx, static_cast<int64_t>(v));
        } else {
            if constexpr (sizeof(T) <= 4)
                return JS_NewUint32(ctx, static_cast<uint32_t>(v));
            else  // uint64 – precision loss for values > 2^53
                return JS_NewFloat64(ctx, static_cast<double>(v));
        }
    }

    static T from_js(JSContext* ctx, JSValueConst v) {
        if constexpr (std::is_signed_v<T>) {
            if constexpr (sizeof(T) <= 4) {
                int32_t i = 0; JS_ToInt32(ctx, &i, v); return static_cast<T>(i);
            } else {
                int64_t i = 0; JS_ToInt64(ctx, &i, v); return static_cast<T>(i);
            }
        } else {
            if constexpr (sizeof(T) <= 4) {
                uint32_t i = 0; JS_ToUint32(ctx, &i, v); return static_cast<T>(i);
            } else {
                double d = 0; JS_ToFloat64(ctx, &d, v); return static_cast<T>(d);
            }
        }
    }

    static bool accept(JSContext* /*ctx*/, JSValueConst v) { return JS_IsNumber(v); }
};

// ── Floating-point ────────────────────────────────────────────────────────────

template <typename T>
struct Converter<T, std::enable_if_t<std::is_floating_point_v<T>>> {
    static JSValue to_js(JSContext* ctx, T v) {
        return JS_NewFloat64(ctx, static_cast<double>(v));
    }
    static T from_js(JSContext* ctx, JSValueConst v) {
        double d = 0; JS_ToFloat64(ctx, &d, v); return static_cast<T>(d);
    }
    static bool accept(JSContext* /*ctx*/, JSValueConst v) { return JS_IsNumber(v); }
};

// ── Enums (scoped and unscoped) ───────────────────────────────────────────────

template <typename T>
struct Converter<T, std::enable_if_t<std::is_enum_v<T>>> {
    using U = std::underlying_type_t<T>;
    static JSValue to_js(JSContext* ctx, T v) {
        return Converter<U>::to_js(ctx, static_cast<U>(v));
    }
    static T from_js(JSContext* ctx, JSValueConst v) {
        return static_cast<T>(Converter<U>::from_js(ctx, v));
    }
    static bool accept(JSContext* ctx, JSValueConst v) { return Converter<U>::accept(ctx, v); }
};

// ── std::string ───────────────────────────────────────────────────────────────

template <>
struct Converter<std::string> {
    static JSValue to_js(JSContext* ctx, const std::string& v) {
        return JS_NewStringLen(ctx, v.data(), v.size());
    }
    static std::string from_js(JSContext* ctx, JSValueConst v) {
        size_t len;
        const char* s = JS_ToCStringLen(ctx, &len, v);
        if (!s) return {};
        std::string r(s, len);
        JS_FreeCString(ctx, s);
        return r;
    }
    static bool accept(JSContext* /*ctx*/, JSValueConst v) { return JS_IsString(v); }
};

// ── std::string_view (to_js only – lifetime unsafe for from_js) ──────────────

template <>
struct Converter<std::string_view> {
    static JSValue to_js(JSContext* ctx, std::string_view v) {
        return JS_NewStringLen(ctx, v.data(), v.size());
    }
};

// ── const char* ───────────────────────────────────────────────────────────────

template <>
struct Converter<const char*> {
    static JSValue to_js(JSContext* ctx, const char* v) {
        return v ? JS_NewString(ctx, v) : JS_NULL;
    }
    // No from_js: returning raw char* is unsafe across the C++/JS boundary.
    // Use Converter<std::string>::from_js instead.
};

// ── std::vector<T> ────────────────────────────────────────────────────────────

template <typename T>
struct Converter<std::vector<T>> {
    static JSValue to_js(JSContext* ctx, const std::vector<T>& v) {
        JSValue arr = JS_NewArray(ctx);
        for (uint32_t i = 0; i < static_cast<uint32_t>(v.size()); ++i)
            JS_SetPropertyUint32(ctx, arr, i, Converter<T>::to_js(ctx, v[i]));
        return arr;
    }

    static std::vector<T> from_js(JSContext* ctx, JSValueConst v) {
        if (!JS_IsArray(ctx, v)) throw TypeError("Expected JS array");
        int64_t len = 0;
        {
            JSValue lv = JS_GetPropertyStr(ctx, v, "length");
            JS_ToInt64(ctx, &len, lv);
            JS_FreeValue(ctx, lv);
        }
        std::vector<T> result;
        result.reserve(static_cast<size_t>(len));
        for (int64_t i = 0; i < len; ++i) {
            JSValue elem = JS_GetPropertyUint32(ctx, v, static_cast<uint32_t>(i));
            result.push_back(Converter<T>::from_js(ctx, elem));
            JS_FreeValue(ctx, elem);
        }
        return result;
    }

    static bool accept(JSContext* ctx, JSValueConst v) { return JS_IsArray(ctx, v) == 1; }
};

// ── Internal helper: object ↔ map conversion ─────────────────────────────────

namespace detail {

template <typename Map, typename K, typename V>
inline JSValue map_to_js(JSContext* ctx, const Map& m) {
    JSValue obj = JS_NewObject(ctx);
    for (const auto& [k, v] : m) {
        JSValue kv = Converter<K>::to_js(ctx, k);
        JSAtom  at = JS_ValueToAtom(ctx, kv);
        JS_FreeValue(ctx, kv);
        JS_DefinePropertyValue(ctx, obj, at, Converter<V>::to_js(ctx, v), JS_PROP_C_W_E);
        JS_FreeAtom(ctx, at);
    }
    return obj;
}

template <typename Map, typename K, typename V>
inline Map map_from_js(JSContext* ctx, JSValueConst v) {
    if (!JS_IsObject(v)) throw TypeError("Expected JS object");
    Map result;
    JSPropertyEnum* props;
    uint32_t count;
    if (JS_GetOwnPropertyNames(ctx, &props, &count, v,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
        return result;
    for (uint32_t i = 0; i < count; ++i) {
        JSValue kv = JS_AtomToValue(ctx, props[i].atom);
        JSValue vv = JS_GetProperty(ctx, v, props[i].atom);
        result.emplace(Converter<K>::from_js(ctx, kv), Converter<V>::from_js(ctx, vv));
        JS_FreeValue(ctx, kv);
        JS_FreeValue(ctx, vv);
        JS_FreeAtom(ctx, props[i].atom);
    }
    js_free(ctx, props);
    return result;
}

} // namespace detail

// ── std::map<K, V> ───────────────────────────────────────────────────────────

template <typename K, typename V>
struct Converter<std::map<K, V>> {
    static JSValue        to_js(JSContext* ctx, const std::map<K, V>& m)  { return detail::map_to_js<std::map<K,V>, K, V>(ctx, m); }
    static std::map<K, V> from_js(JSContext* ctx, JSValueConst v)          { return detail::map_from_js<std::map<K,V>, K, V>(ctx, v); }
    static bool           accept(JSContext* /*ctx*/, JSValueConst v)       { return JS_IsObject(v); }
};

// ── std::unordered_map<K, V> ─────────────────────────────────────────────────

template <typename K, typename V>
struct Converter<std::unordered_map<K, V>> {
    static JSValue                    to_js(JSContext* ctx, const std::unordered_map<K, V>& m) { return detail::map_to_js<std::unordered_map<K,V>, K, V>(ctx, m); }
    static std::unordered_map<K, V> from_js(JSContext* ctx, JSValueConst v)                    { return detail::map_from_js<std::unordered_map<K,V>, K, V>(ctx, v); }
    static bool                     accept(JSContext* /*ctx*/, JSValueConst v)                 { return JS_IsObject(v); }
};

// ── Internal helper: array-like → set conversion ─────────────────────────────

namespace detail {

template <typename Set, typename T>
inline JSValue set_to_js(JSContext* ctx, const Set& s) {
    JSValue arr = JS_NewArray(ctx);
    uint32_t i = 0;
    for (const auto& elem : s)
        JS_SetPropertyUint32(ctx, arr, i++, Converter<T>::to_js(ctx, elem));
    return arr;
}

template <typename Set, typename T>
inline Set set_from_js(JSContext* ctx, JSValueConst v) {
    if (!JS_IsArray(ctx, v)) throw TypeError("Expected JS array");
    Set result;
    int64_t len = 0;
    {
        JSValue lv = JS_GetPropertyStr(ctx, v, "length");
        JS_ToInt64(ctx, &len, lv);
        JS_FreeValue(ctx, lv);
    }
    for (int64_t i = 0; i < len; ++i) {
        JSValue elem = JS_GetPropertyUint32(ctx, v, static_cast<uint32_t>(i));
        result.insert(Converter<T>::from_js(ctx, elem));
        JS_FreeValue(ctx, elem);
    }
    return result;
}

} // namespace detail

// ── std::set<T> ──────────────────────────────────────────────────────────────

template <typename T>
struct Converter<std::set<T>> {
    static JSValue     to_js(JSContext* ctx, const std::set<T>& s)  { return detail::set_to_js<std::set<T>, T>(ctx, s); }
    static std::set<T> from_js(JSContext* ctx, JSValueConst v)       { return detail::set_from_js<std::set<T>, T>(ctx, v); }
};

// ── std::unordered_set<T> ────────────────────────────────────────────────────

template <typename T>
struct Converter<std::unordered_set<T>> {
    static JSValue              to_js(JSContext* ctx, const std::unordered_set<T>& s) { return detail::set_to_js<std::unordered_set<T>, T>(ctx, s); }
    static std::unordered_set<T> from_js(JSContext* ctx, JSValueConst v)               { return detail::set_from_js<std::unordered_set<T>, T>(ctx, v); }
};

// ── std::array<T, N> ─────────────────────────────────────────────────────────

template <typename T, std::size_t N>
struct Converter<std::array<T, N>> {
    static JSValue            to_js(JSContext* ctx, const std::array<T, N>& a) {
        JSValue arr = JS_NewArray(ctx);
        for (uint32_t i = 0; i < static_cast<uint32_t>(N); ++i)
            JS_SetPropertyUint32(ctx, arr, i, Converter<T>::to_js(ctx, a[i]));
        return arr;
    }
    static std::array<T, N> from_js(JSContext* ctx, JSValueConst v) {
        if (!JS_IsArray(ctx, v)) throw TypeError("Expected JS array");
        std::array<T, N> result{};
        for (uint32_t i = 0; i < static_cast<uint32_t>(N); ++i) {
            JSValue elem = JS_GetPropertyUint32(ctx, v, i);
            result[i] = Converter<T>::from_js(ctx, elem);
            JS_FreeValue(ctx, elem);
        }
        return result;
    }
};

// ── std::optional<T> ─────────────────────────────────────────────────────────

template <typename T>
struct Converter<std::optional<T>> {
    static JSValue to_js(JSContext* ctx, const std::optional<T>& opt) {
        if (!opt.has_value()) return JS_NULL;
        return Converter<T>::to_js(ctx, *opt);
    }
    static std::optional<T> from_js(JSContext* ctx, JSValueConst v) {
        if (JS_IsNull(v) || JS_IsUndefined(v)) return std::nullopt;
        return Converter<T>::from_js(ctx, v);
    }
};

// ── Raw JSValue passthrough ───────────────────────────────────────────────────

template <>
struct Converter<JSValue> {
    static JSValue to_js(JSContext* ctx, JSValue v)         { return JS_DupValue(ctx, v); }
    static JSValue from_js(JSContext* ctx, JSValueConst v)  { return JS_DupValue(ctx, v); }
};

// ── qjsb::Value passthrough ──────────────────────────────────────────────────

template <>
struct Converter<Value> {
    static JSValue to_js(JSContext* /*ctx*/, Value v)          { return v.steal(); }
    static Value   from_js(JSContext* ctx, JSValueConst v)     { return Value::dup(ctx, v); }
};

} // namespace qjsb
