// test_convert.cpp – tests for Converter<T> type conversions
#include <qjsbridge.hpp>
#include <cassert>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <array>
#include <optional>

using namespace qjsb;

// ── round-trip helpers ────────────────────────────────────────────────────────

template <typename T>
static T round_trip(JSContext* ctx, T val) {
    JSValue js = to_js(ctx, val);
    T result = from_js<T>(ctx, js);
    JS_FreeValue(ctx, js);
    return result;
}

// ── bool ─────────────────────────────────────────────────────────────────────

static void test_bool() {
    Runtime rt; Context ctx(rt);
    assert(round_trip(ctx.get(), true)  == true);
    assert(round_trip(ctx.get(), false) == false);
}

// ── integers ─────────────────────────────────────────────────────────────────

static void test_integers() {
    Runtime rt; Context ctx(rt);
    assert(round_trip<int8_t>(ctx.get(), -120) == -120);
    assert(round_trip<uint8_t>(ctx.get(), 200) == 200);
    assert(round_trip<int16_t>(ctx.get(), -30000) == -30000);
    assert(round_trip<uint16_t>(ctx.get(), 60000) == 60000);
    assert(round_trip<int32_t>(ctx.get(), -1234567) == -1234567);
    assert(round_trip<uint32_t>(ctx.get(), 3000000000u) == 3000000000u);
    assert(round_trip<int64_t>(ctx.get(), -9000000000LL) == -9000000000LL);
    // char / short / long
    assert(round_trip<char>(ctx.get(), 'A') == 'A');
    assert(round_trip<long>(ctx.get(), 100L) == 100L);
}

// ── floating point ────────────────────────────────────────────────────────────

static void test_floats() {
    Runtime rt; Context ctx(rt);
    assert(std::abs(round_trip<float>(ctx.get(), 3.14f) - 3.14f) < 0.001f);
    assert(std::abs(round_trip<double>(ctx.get(), 2.718281828) - 2.718281828) < 1e-9);
}

// ── enum ─────────────────────────────────────────────────────────────────────

enum class Color { Red = 0, Green = 1, Blue = 2 };

static void test_enum() {
    Runtime rt; Context ctx(rt);
    assert(round_trip(ctx.get(), Color::Green) == Color::Green);
    assert(round_trip(ctx.get(), Color::Blue)  == Color::Blue);
}

// ── std::string ───────────────────────────────────────────────────────────────

static void test_string() {
    Runtime rt; Context ctx(rt);
    std::string s = "hello, qjsbridge!";
    assert(round_trip(ctx.get(), s) == s);
    assert(round_trip(ctx.get(), std::string("")) == "");
    // Unicode
    std::string utf8 = u8"\u4e2d\u6587";
    assert(round_trip(ctx.get(), utf8) == utf8);
}

// ── const char* → JS (no from_js) ────────────────────────────────────────────

static void test_cstring_to_js() {
    Runtime rt; Context ctx(rt);
    JSValue v = to_js(ctx.get(), "static string");
    assert(JS_IsString(v));
    std::string s = Converter<std::string>::from_js(ctx.get(), v);
    assert(s == "static string");
    JS_FreeValue(ctx.get(), v);

    JSValue nv = to_js(ctx.get(), static_cast<const char*>(nullptr));
    assert(JS_IsNull(nv));
    JS_FreeValue(ctx.get(), nv);
}

// ── std::vector<T> ───────────────────────────────────────────────────────────

static void test_vector() {
    Runtime rt; Context ctx(rt);
    std::vector<int> v = {1, 2, 3, 4, 5};
    auto v2 = round_trip(ctx.get(), v);
    assert(v2 == v);

    std::vector<std::string> sv = {"one", "two", "three"};
    auto sv2 = round_trip(ctx.get(), sv);
    assert(sv2 == sv);

    // Empty vector
    std::vector<double> empty;
    auto empty2 = round_trip(ctx.get(), empty);
    assert(empty2.empty());
}

// ── std::map<K, V> ───────────────────────────────────────────────────────────

static void test_map() {
    Runtime rt; Context ctx(rt);
    std::map<std::string, int> m = {{"a", 1}, {"b", 2}, {"c", 3}};
    auto m2 = round_trip(ctx.get(), m);
    assert(m2 == m);
}

// ── std::set<T> ──────────────────────────────────────────────────────────────

static void test_set() {
    Runtime rt; Context ctx(rt);
    std::set<int> s = {5, 3, 1, 4, 2};
    std::set<int> s2 = round_trip(ctx.get(), s);
    assert(s2 == s);
}

// ── std::array<T, N> ─────────────────────────────────────────────────────────

static void test_array() {
    Runtime rt; Context ctx(rt);
    std::array<int, 4> a = {10, 20, 30, 40};
    auto a2 = round_trip(ctx.get(), a);
    assert(a2 == a);
}

// ── std::optional<T> ─────────────────────────────────────────────────────────

static void test_optional() {
    Runtime rt; Context ctx(rt);
    std::optional<int> present = 42;
    std::optional<int> absent  = std::nullopt;

    auto p2 = round_trip(ctx.get(), present);
    auto a2 = round_trip(ctx.get(), absent);

    assert(p2.has_value() && *p2 == 42);
    assert(!a2.has_value());
}

// ── nested containers ─────────────────────────────────────────────────────────

static void test_nested() {
    Runtime rt; Context ctx(rt);
    std::vector<std::vector<int>> nested = {{1, 2}, {3, 4}, {5}};
    auto n2 = round_trip(ctx.get(), nested);
    assert(n2 == nested);

    std::map<std::string, std::vector<int>> m = {{"x", {1, 2}}, {"y", {3}}};
    auto m2 = round_trip(ctx.get(), m);
    assert(m2 == m);
}

// ── JSValue passthrough ───────────────────────────────────────────────────────

static void test_jsvalue_passthrough() {
    Runtime rt; Context ctx(rt);
    JSValue orig = JS_NewInt32(ctx.get(), 77);
    JSValue dup  = to_js(ctx.get(), orig);
    JS_FreeValue(ctx.get(), orig);
    int32_t n = 0;
    JS_ToInt32(ctx.get(), &n, dup);
    assert(n == 77);
    JS_FreeValue(ctx.get(), dup);
}

// ── Value passthrough ─────────────────────────────────────────────────────────

static void test_value_passthrough() {
    Runtime rt; Context ctx(rt);
    Value v(ctx.get(), JS_NewInt32(ctx.get(), 55));
    Value v2 = from_js<Value>(ctx.get(), v.get());
    int32_t n = 0;
    JS_ToInt32(ctx.get(), &n, v2.get());
    assert(n == 55);
}

int main() {
    test_bool();
    test_integers();
    test_floats();
    test_enum();
    test_string();
    test_cstring_to_js();
    test_vector();
    test_map();
    test_set();
    test_array();
    test_optional();
    test_nested();
    test_jsvalue_passthrough();
    test_value_passthrough();
    return 0;
}
