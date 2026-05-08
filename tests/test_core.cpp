// test_core.cpp – tests for Runtime, Context, and Value RAII wrappers
#include <qjsbridge.hpp>
#include <cassert>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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

static void test_default_module_loader_reports_missing_module() {
    Runtime rt;
    Context ctx(rt);

    bool caught = false;
    try {
        ctx.eval(R"(
            import "./invalid_module.js";
        )", "<eval>", JS_EVAL_TYPE_MODULE);
    } catch (const JSException& e) {
        caught = true;
        assert(std::string(e.what()) ==
               "ReferenceError: could not load module filename 'invalid_module.js'");
    }
    assert(caught);
}

static void test_quickjspp_style_module_loader() {
    Runtime rt;
    Context ctx(rt);

    std::unordered_map<std::string, std::string> files = {
        {
            "some_module.js",
            R"(
                import "folder/file1.js";
                log(import.meta.url);
            )"
        },
        {
            "folder/file1.js",
            R"(
                import "./file2.js";
                log(import.meta.url);
            )"
        },
        {
            "folder/file2.js",
            R"(
                import "http://localhost/script1.js";
                log(import.meta.url);
            )"
        },
        {
            "http://localhost/script1.js",
            R"(
                import "./script2.js";
                log(import.meta.url);
            )"
        },
        {
            "http://localhost/script2.js",
            R"(
                log(import.meta.url);
            )"
        },
    };
    std::vector<std::string> logged_urls;

    ctx.moduleLoader = [&files](std::string_view filename) -> ModuleData {
        auto it = files.find(std::string(filename));
        if (it != files.end())
            return ModuleData{detail::toUri(filename), it->second};
        return {};
    };
    ctx.bindFunction("log", [&logged_urls](std::string s) {
        logged_urls.push_back(std::move(s));
    });

    ctx.eval(R"(
        import "./some_module.js";
    )", "<eval>", JS_EVAL_TYPE_MODULE);

    const std::vector<std::string> expected = {
        detail::toUri("some_module.js"),
        detail::toUri("folder/file1.js"),
        detail::toUri("folder/file2.js"),
        "http://localhost/script1.js",
        "http://localhost/script2.js",
    };
    assert(logged_urls == expected);
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
    test_default_module_loader_reports_missing_module();
    test_quickjspp_style_module_loader();
    return 0;
}
