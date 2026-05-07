// test_core.cpp – tests for Runtime, Context, and Value RAII wrappers
#include <qjsbridge.hpp>
#include <cassert>
#include <string>

using namespace qjsb;

static void test_runtime_context_creation() {
    Runtime rt;
    Context ctx(rt);
    assert(rt.get() != nullptr);
    assert(ctx.get() != nullptr);
}

static void test_eval_returns_value() {
    Runtime rt;
    Context ctx(rt);
    Value v = ctx.eval("1 + 2");
    assert(v.isNumber());
    // Convert to int via JS
    int32_t n = 0;
    JS_ToInt32(ctx.get(), &n, v.get());
    assert(n == 3);
}

static void test_eval_string() {
    Runtime rt;
    Context ctx(rt);
    Value v = ctx.eval("'hello' + ' world'");
    assert(v.isString());
    assert(v.toString() == "hello world");
}

static void test_eval_bool() {
    Runtime rt;
    Context ctx(rt);
    Value t = ctx.eval("true");
    Value f = ctx.eval("false");
    assert(t.isBool());
    assert(f.isBool());
    assert(JS_ToBool(ctx.get(), t.get()) == 1);
    assert(JS_ToBool(ctx.get(), f.get()) == 0);
}

static void test_eval_throws_on_error() {
    Runtime rt;
    Context ctx(rt);
    bool caught = false;
    try {
        ctx.eval("(syntax error)");
    } catch (const JSException&) {
        caught = true;
    }
    assert(caught);
}

static void test_global_set_get() {
    Runtime rt;
    Context ctx(rt);
    ctx.eval("var x = 42;");
    Value x = ctx.getGlobal("x");
    assert(x.isNumber());
    int32_t n = 0;
    JS_ToInt32(ctx.get(), &n, x.get());
    assert(n == 42);
}

static void test_value_copy_move() {
    Runtime rt;
    Context ctx(rt);
    Value a = ctx.eval("99");
    Value b = a;                    // copy: dup
    Value c = std::move(b);         // move: steal

    assert(a.isNumber());
    assert(c.isNumber());
    assert(b.isUndefined());        // moved-from state

    int32_t na = 0, nc = 0;
    JS_ToInt32(ctx.get(), &na, a.get());
    JS_ToInt32(ctx.get(), &nc, c.get());
    assert(na == 99);
    assert(nc == 99);
}

static void test_value_property_access() {
    Runtime rt;
    Context ctx(rt);
    Value obj = ctx.eval("({a: 1, b: 'two'})");
    assert(obj.isObject());
    Value a = obj["a"];
    Value b = obj["b"];
    assert(a.isNumber());
    assert(b.isString());
    assert(b.toString() == "two");
}

static void test_value_array_length() {
    Runtime rt;
    Context ctx(rt);
    Value arr = ctx.eval("[10, 20, 30]");
    assert(arr.isArray());
    assert(arr.length() == 3);
    Value elem = arr[1];
    int32_t n = 0;
    JS_ToInt32(ctx.get(), &n, elem.get());
    assert(n == 20);
}

static void test_runtime_gc() {
    Runtime rt;
    rt.runGC();   // Should not crash.
}

int main() {
    test_runtime_context_creation();
    test_eval_returns_value();
    test_eval_string();
    test_eval_bool();
    test_eval_throws_on_error();
    test_global_set_get();
    test_value_copy_move();
    test_value_property_access();
    test_value_array_length();
    test_runtime_gc();
    return 0;
}
