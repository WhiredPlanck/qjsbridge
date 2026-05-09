// test_class.cpp – tests for C++ class / struct binding
#include <qjsbridge.hpp>
#include <cassert>
#include <memory>
#include <string>

using namespace qjsb;

// ── Minimal value-type class ──────────────────────────────────────────────────

struct Point {
    double x, y;
    Point(double x, double y) : x(x), y(y) {}
    double length() const { return x * x + y * y; }
    void   scale(double s) { x *= s; y *= s; }
    std::string str() const {
        return "(" + std::to_string(x) + ", " + std::to_string(y) + ")";
    }
};

static void test_point_binding() {
    Runtime rt;
    Context ctx(rt);

    ClassDef<Point>(ctx, "Point")
        .constructor<double, double>()
        .field("x", &Point::x)
        .field("y", &Point::y)
        .method("length", &Point::length)
        .method("scale",  &Point::scale)
        .method("str",    &Point::str)
        .endClassGlobal();

    // Construction
    Value p = ctx.eval("new Point(3, 4)");
    assert(p.isObject());

    // Field getter
    Value xv = ctx.eval("(new Point(3, 4)).x");
    double xd = 0; JS_ToFloat64(ctx.get(), &xd, xv.get());
    assert(xd == 3.0);

    // Method call
    Value len = ctx.eval("(new Point(3, 4)).length()");
    double ld = 0; JS_ToFloat64(ctx.get(), &ld, len.get());
    assert(ld == 25.0);

    // Field setter via assignment
    ctx.eval("var p2 = new Point(1, 0); p2.x = 5;");
    Value p2x = ctx.eval("p2.x");
    double p2xd = 0; JS_ToFloat64(ctx.get(), &p2xd, p2x.get());
    assert(p2xd == 5.0);

    // void method mutation
    ctx.eval("var p3 = new Point(2, 3); p3.scale(2);");
    Value p3x = ctx.eval("p3.x");
    double p3xd = 0; JS_ToFloat64(ctx.get(), &p3xd, p3x.get());
    assert(p3xd == 4.0);

    // String method
    Value s = ctx.eval("new Point(1, 2).str()");
    assert(s.isString());
}

// ── Class with static method ──────────────────────────────────────────────────

struct Counter {
    int count = 0;
    Counter() = default;
    explicit Counter(int start) : count(start) {}
    void increment() { ++count; }
    void add(int n)  { count += n; }
    int  value() const { return count; }
    static Counter zero() { return Counter(0); }
};

static void test_counter_static() {
    Runtime rt; Context ctx(rt);

    ClassDef<Counter>(ctx, "Counter")
        .constructor<int>()
        .method("increment", &Counter::increment)
        .method("add",       &Counter::add)
        .method("value",     &Counter::value)
        .staticMethod("zero", Counter::zero)
        .endClassGlobal();

    Value v = ctx.eval(R"(
        var c = new Counter(10);
        c.increment();
        c.add(5);
        c.value()
    )");
    int32_t n = 0; JS_ToInt32(ctx.get(), &n, v.get());
    assert(n == 16);

    // Static method
    Value z = ctx.eval("Counter.zero().value()");
    int32_t zn = 0; JS_ToInt32(ctx.get(), &zn, z.get());
    assert(zn == 0);
}

// ── property(name, getter) (read-only overload) ──────────────────────────────

struct Circle {
    double radius;
    explicit Circle(double r) : radius(r) {}
    double area()  const { return 3.14159265358979 * radius * radius; }
    double perimeter() const { return 2.0 * 3.14159265358979 * radius; }
};

static void test_readonly_property_overload() {
    Runtime rt; Context ctx(rt);

    ClassDef<Circle>(ctx, "Circle")
        .constructor<double>()
        .field("radius",    &Circle::radius)
        .property("area",      &Circle::area)
        .property("perimeter", &Circle::perimeter)
        .endClassGlobal();

    Value area = ctx.eval("new Circle(1).area");
    double ad = 0; JS_ToFloat64(ctx.get(), &ad, area.get());
    assert(std::abs(ad - 3.14159265358979) < 1e-8);
}

// ── Lambda-style method (T* first arg) ───────────────────────────────────────

struct StringHolder {
    std::string data;
    explicit StringHolder(std::string s) : data(std::move(s)) {}
};

static void test_lambda_method() {
    Runtime rt; Context ctx(rt);

    ClassDef<StringHolder>(ctx, "StringHolder")
        .constructor<std::string>()
        .field("data", &StringHolder::data)
        .method("upper", [](StringHolder* self) -> std::string {
            std::string r = self->data;
            for (char& c : r) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
            return r;
        })
        .method("append", [](StringHolder* self, std::string s) {
            self->data += s;
        })
        .endClassGlobal();

    Value v = ctx.eval("new StringHolder('hello').upper()");
    assert(v.toString() == "HELLO");

    ctx.eval("var sh = new StringHolder('foo'); sh.append(' bar');");
    Value d = ctx.eval("sh.data");
    assert(d.toString() == "foo bar");
}

// ── pushOwned / pushBorrowed ──────────────────────────────────────────────────

static void test_push_owned() {
    Runtime rt; Context ctx(rt);

    // Register the class (constructor not required for pushOwned)
    ClassDef<Point>(ctx, "Point")
        .constructor<double, double>()
        .field("x", &Point::x)
        .field("y", &Point::y)
        .method("length", &Point::length)
        .endClassGlobal();

    // Create from C++ and push to JS (JS takes ownership)
    auto* p = new Point(5.0, 12.0);
    ctx.setGlobal("cppPoint", pushOwned(ctx, p));

    Value len = ctx.eval("cppPoint.length()");
    double d = 0; JS_ToFloat64(ctx.get(), &d, len.get());
    assert(d == 169.0);   // 5² + 12² = 169
}

static void test_push_borrowed() {
    Runtime rt; Context ctx(rt);

    ClassDef<Point>(ctx, "Point")
        .constructor<double, double>()
        .field("x", &Point::x)
        .field("y", &Point::y)
        .endClassGlobal();

    Point p(3.0, 4.0);
    ctx.setGlobal("bp", pushBorrowed(ctx, &p));

    ctx.eval("bp.x = 10;");  // mutate through borrowed reference
    assert(p.x == 10.0);    // C++ object is updated
}

// ── std::shared_ptr binding ───────────────────────────────────────────────────

static void test_shared_ptr() {
    Runtime rt; Context ctx(rt);

    ClassDef<Counter>(ctx, "Counter")
        .constructor<int>()
        .method("increment", &Counter::increment)
        .method("value",     &Counter::value)
        .endClassGlobal();

    auto sp = std::make_shared<Counter>(7);
    ctx.setGlobal("sc", pushShared(ctx, sp));

    ctx.eval("sc.increment(); sc.increment();");
    assert(sp->value() == 9);   // same object via shared_ptr
}

// ── explicit property with getter+setter ─────────────────────────────────────

struct Rectangle {
    double w, h;
    Rectangle(double w, double h) : w(w), h(h) {}
    double getArea() const { return w * h; }
    double getW()  const { return w; }
    void   setW(double v) { w = v; }
};

struct OverloadDemo {
    int v{0};
    int set(int x) { v = x; return v; }
    std::string set(const std::string& s) { v = static_cast<int>(s.size()); return s; }
    int get() const { return v; }
};

static void test_property() {
    Runtime rt; Context ctx(rt);

    ClassDef<Rectangle>(ctx, "Rectangle")
        .constructor<double, double>()
        .field("h", &Rectangle::h)
        .property("w", &Rectangle::getW, &Rectangle::setW)
        .property("area", &Rectangle::getArea)
        .endClassGlobal();

    ctx.eval("var r = new Rectangle(3, 4);");
    Value w = ctx.eval("r.w");
    double wd = 0; JS_ToFloat64(ctx.get(), &wd, w.get());
    assert(wd == 3.0);

    ctx.eval("r.w = 6;");
    Value area = ctx.eval("r.area");
    double ad = 0; JS_ToFloat64(ctx.get(), &ad, area.get());
    assert(ad == 24.0);
}

// ── Multiple classes in same context ─────────────────────────────────────────

static void test_multiple_classes() {
    Runtime rt; Context ctx(rt);

    ClassDef<Point>(ctx, "Point")
        .constructor<double, double>()
        .field("x", &Point::x)
        .field("y", &Point::y)
        .endClassGlobal();

    ClassDef<Counter>(ctx, "Counter")
        .constructor<int>()
        .method("value", &Counter::value)
        .endClassGlobal();

    ctx.eval("var p = new Point(1, 2); var c = new Counter(10);");
    Value cx = ctx.eval("p.x");
    double xd = 0; JS_ToFloat64(ctx.get(), &xd, cx.get());
    assert(xd == 1.0);

    Value cv = ctx.eval("c.value()");
    int32_t n = 0; JS_ToInt32(ctx.get(), &n, cv.get());
    assert(n == 10);
}

static void test_module_class_export() {
    Runtime rt; Context ctx(rt);
    auto module = ctx.newModule("geom");

    ClassDef<Point>(ctx, "Point")
        .constructor<double, double>()
        .field("x", &Point::x)
        .field("y", &Point::y)
        .method("length", &Point::length)
        .endClass(module);

    Value v = ctx.evalModule(R"(
        import { Point } from "geom";
        globalThis.moduleLen = new Point(6, 8).length();
    )", "module_test.mjs");
    (void)v;

    Value len = ctx.eval("moduleLen");
    double d = 0; JS_ToFloat64(ctx.get(), &d, len.get());
    assert(d == 100.0);
}

static void test_raw_method_manual_overload_dispatch() {
    Runtime rt; Context ctx(rt);

    ClassDef<OverloadDemo>(ctx, "OverloadDemo")
        .constructor<>()
        .method("set", [](JSContext* js,
                          OverloadDemo* self,
                          int argc,
                          JSValueConst* argv) -> JSValue {
            if (argc == 1 && JS_IsNumber(argv[0])) {
                int32_t x = 0;
                JS_ToInt32(js, &x, argv[0]);
                return JS_NewInt32(js, self->set(static_cast<int>(x)));
            }
            if (argc == 1 && JS_IsString(argv[0])) {
                const char* s = JS_ToCString(js, argv[0]);
                if (!s) return JS_EXCEPTION;
                std::string in(s);
                JS_FreeCString(js, s);
                std::string out = self->set(in);
                return JS_NewStringLen(js, out.c_str(), out.size());
            }
            return JS_ThrowTypeError(js, "No matching overload for OverloadDemo.set()");
        })
        .method("get", &OverloadDemo::get)
        .endClassGlobal();

    Value n = ctx.eval("var o = new OverloadDemo(); o.set(12);");
    int32_t ni = 0; JS_ToInt32(ctx.get(), &ni, n.get());
    assert(ni == 12);

    Value s = ctx.eval("o.set('abcd')");
    assert(s.toString() == "abcd");

    Value v = ctx.eval("o.get()");
    int32_t vi = 0; JS_ToInt32(ctx.get(), &vi, v.get());
    assert(vi == 4);
}

struct Animal {
    int age{0};
    explicit Animal(int a) : age(a) {}
};

struct Dog : Animal {
    std::string name;
    Dog(int a, std::string n) : Animal(a), name(std::move(n)) {}
    std::string bark() const { return "woof:" + name; }
};

static void test_module_begin_derive_and_chain() {
    Runtime rt; Context ctx(rt);
    auto module = ctx.newModule("zoo");

    module.beginClass<Animal>("Animal")
        .constructor<int>()
        .field("age", &Animal::age)
        .endClass()
        .deriveClass<Dog, Animal>("Dog")
        .constructor<int, std::string>()
        .field("name", &Dog::name)
        .method("bark", &Dog::bark)
        .endClass();

    ctx.evalModule(R"(
        import { Animal, Dog } from "zoo";
        const d = new Dog(3, "fido");
        globalThis.dogAge = d.age;
        globalThis.dogBark = d.bark();
        globalThis.dogIsAnimal = d instanceof Animal;
    )", "zoo_test.mjs");

    int32_t age = 0;
    JS_ToInt32(ctx.get(), &age, ctx.eval("dogAge").get());
    assert(age == 3);
    assert(ctx.eval("dogBark").toString() == "woof:fido");
    assert(JS_ToBool(ctx.get(), ctx.eval("dogIsAnimal").get()) == 1);
}

static void test_value_operator_assignment() {
    Runtime rt; Context ctx(rt);

    Value obj = ctx.newObject();
    obj["answer"] = JS_NewInt32(ctx.get(), 42);
    obj["hello"] = [](std::string name) {
        return std::string("hello ") + name;
    };
    ctx.setGlobal("obj", std::move(obj));

    int32_t answer = 0;
    JS_ToInt32(ctx.get(), &answer, ctx.eval("obj.answer").get());
    assert(answer == 42);
    assert(ctx.eval("obj.hello('bridge')").toString() == "hello bridge");
}

int main() {
    test_point_binding();
    test_counter_static();
    test_readonly_property_overload();
    test_lambda_method();
    test_push_owned();
    test_push_borrowed();
    test_shared_ptr();
    test_property();
    test_multiple_classes();
    test_module_class_export();
    test_raw_method_manual_overload_dispatch();
    test_module_begin_derive_and_chain();
    test_value_operator_assignment();
    return 0;
}
