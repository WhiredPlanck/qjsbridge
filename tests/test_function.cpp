// test_function.cpp – tests for function / lambda binding
#include <qjsbridge.hpp>
#include <cassert>
#include <string>
#include <vector>

using namespace qjsb;

// ── free function binding ─────────────────────────────────────────────────────

static int add(int a, int b) { return a + b; }
static std::string greet(const std::string& name) { return "Hello, " + name + "!"; }
static void no_return(int /*x*/) {}

static void test_free_function() {
    Runtime rt; Context ctx(rt);
    ctx.bindFunction("add", add);
    Value result = ctx.eval("add(3, 4)");
    assert(result.isNumber());
    int32_t n = 0;
    JS_ToInt32(ctx.get(), &n, result.get());
    assert(n == 7);
}

static void test_string_function() {
    Runtime rt; Context ctx(rt);
    ctx.bindFunction("greet", greet);
    Value result = ctx.eval("greet('World')");
    assert(result.isString());
    assert(result.toString() == "Hello, World!");
}

static void test_void_function() {
    Runtime rt; Context ctx(rt);
    ctx.bindFunction("no_return", no_return);
    Value result = ctx.eval("no_return(5)");
    assert(result.isUndefined());
}

// ── lambda binding ────────────────────────────────────────────────────────────

static void test_lambda() {
    Runtime rt; Context ctx(rt);
    ctx.bindFunction("multiply", [](int a, int b) { return a * b; });
    Value result = ctx.eval("multiply(6, 7)");
    int32_t n = 0;
    JS_ToInt32(ctx.get(), &n, result.get());
    assert(n == 42);
}

static void test_capturing_lambda() {
    Runtime rt; Context ctx(rt);
    int base = 10;
    ctx.bindFunction("addBase", [base](int x) { return base + x; });
    Value result = ctx.eval("addBase(5)");
    int32_t n = 0;
    JS_ToInt32(ctx.get(), &n, result.get());
    assert(n == 15);
}

// ── no-arg function ───────────────────────────────────────────────────────────

static void test_no_args() {
    Runtime rt; Context ctx(rt);
    ctx.bindFunction("getAnswer", []() { return 42; });
    Value result = ctx.eval("getAnswer()");
    int32_t n = 0;
    JS_ToInt32(ctx.get(), &n, result.get());
    assert(n == 42);
}

// ── container arguments ───────────────────────────────────────────────────────

static void test_vector_arg() {
    Runtime rt; Context ctx(rt);
    ctx.bindFunction("sumVec", [](std::vector<int> v) {
        int s = 0;
        for (int x : v) s += x;
        return s;
    });
    Value result = ctx.eval("sumVec([1, 2, 3, 4])");
    int32_t n = 0;
    JS_ToInt32(ctx.get(), &n, result.get());
    assert(n == 10);
}

static void test_vector_return() {
    Runtime rt; Context ctx(rt);
    ctx.bindFunction("makeRange", [](int n) {
        std::vector<int> v;
        for (int i = 0; i < n; ++i) v.push_back(i);
        return v;
    });
    Value result = ctx.eval("makeRange(4)");
    assert(result.isArray());
    assert(result.length() == 4);
    int32_t v0 = 0, v3 = 0;
    {
        Value e = result[uint32_t{0}]; JS_ToInt32(ctx.get(), &v0, e.get());
        Value e3 = result[uint32_t{3}]; JS_ToInt32(ctx.get(), &v3, e3.get());
    }
    assert(v0 == 0);
    assert(v3 == 3);
}

// ── makeFunction → manual global placement ────────────────────────────────────

static void test_make_function() {
    Runtime rt; Context ctx(rt);
    Value fn = makeFunction(ctx, [](double x) { return x * x; }, "square");
    ctx.setGlobal("square", std::move(fn));
    Value result = ctx.eval("square(9)");
    double d = 0;
    JS_ToFloat64(ctx.get(), &d, result.get());
    assert(d == 81.0);
}

// ── C++ exception propagation to JS ──────────────────────────────────────────

static void test_exception_propagation() {
    Runtime rt; Context ctx(rt);
    ctx.bindFunction("throws_cpp", []() -> int {
        throw std::runtime_error("oops from C++");
        return 0;
    });

    bool caught = false;
    try {
        ctx.eval("throws_cpp()");
    } catch (const JSException& e) {
        caught = true;
        std::string msg = e.what();
        assert(msg.find("oops from C++") != std::string::npos);
    }
    assert(caught);
}

// ── optional argument (default to nullopt) ────────────────────────────────────

static void test_optional_arg() {
    Runtime rt; Context ctx(rt);
    ctx.bindFunction("maybeAdd", [](std::optional<int> a, std::optional<int> b) {
        return (a.value_or(0)) + (b.value_or(0));
    });
    Value r1 = ctx.eval("maybeAdd(3, 4)");
    Value r2 = ctx.eval("maybeAdd(3, null)");
    int32_t n1 = 0, n2 = 0;
    JS_ToInt32(ctx.get(), &n1, r1.get());
    JS_ToInt32(ctx.get(), &n2, r2.get());
    assert(n1 == 7);
    assert(n2 == 3);
}

// ── multiple bindings in same context ────────────────────────────────────────

static void test_multiple_bindings() {
    Runtime rt; Context ctx(rt);
    ctx.bindFunction("f1", [](int x) { return x + 1; });
    ctx.bindFunction("f2", [](int x) { return x * 2; });
    ctx.bindFunction("f3", [](int x) { return x - 1; });
    Value result = ctx.eval("f3(f2(f1(4)))");
    int32_t n = 0;
    JS_ToInt32(ctx.get(), &n, result.get());
    assert(n == 9);  // f3(f2(5)) = f3(10) = 9
}

static void test_module_function_binding() {
    Runtime rt; Context ctx(rt);
    auto module = ctx.newModule("math_ext");
    module.bindFunction("add", [](int a, int b) { return a + b; });
    module.bindFunction("mul", [](int a, int b) { return a * b; });

    ctx.evalModule(R"(
        import { add, mul } from "math_ext";
        globalThis.moduleResult = mul(add(1, 2), 5);
    )", "module_func_test.mjs");

    Value v = ctx.eval("moduleResult");
    int32_t n = 0;
    JS_ToInt32(ctx.get(), &n, v.get());
    assert(n == 15);
}

static void test_raw_function_manual_overload_dispatch() {
    Runtime rt; Context ctx(rt);
    ctx.bindFunctionRaw("overload", [](JSContext* js,
                                       JSValueConst /*this_val*/,
                                       int argc,
                                       JSValueConst* argv) -> JSValue {
        if (argc == 2 && JS_IsNumber(argv[0]) && JS_IsNumber(argv[1])) {
            int32_t a = 0, b = 0;
            JS_ToInt32(js, &a, argv[0]);
            JS_ToInt32(js, &b, argv[1]);
            return JS_NewInt32(js, a + b);
        }
        if (argc == 1 && JS_IsString(argv[0])) {
            const char* s = JS_ToCString(js, argv[0]);
            if (!s) return JS_EXCEPTION;
            std::string out = std::string("hello ") + s;
            JS_FreeCString(js, s);
            return JS_NewStringLen(js, out.c_str(), out.size());
        }
        return JS_ThrowTypeError(js, "No matching overload for overload()");
    });

    Value n = ctx.eval("overload(7, 8)");
    int32_t ni = 0;
    JS_ToInt32(ctx.get(), &ni, n.get());
    assert(ni == 15);

    Value s = ctx.eval("overload('qjs')");
    assert(s.toString() == "hello qjs");
}

int main() {
    test_free_function();
    test_string_function();
    test_void_function();
    test_lambda();
    test_capturing_lambda();
    test_no_args();
    test_vector_arg();
    test_vector_return();
    test_make_function();
    test_exception_propagation();
    test_optional_arg();
    test_multiple_bindings();
    test_module_function_binding();
    test_raw_function_manual_overload_dispatch();
    return 0;
}
