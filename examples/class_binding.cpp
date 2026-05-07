// examples/class_binding.cpp – binding a C++ class to JavaScript
//
// Demonstrates:
//   • ClassDef<T> fluent API
//   • Constructor, methods, data fields
//   • Static methods
//   • pushOwned / pushBorrowed helpers

#include <qjsbridge.hpp>
#include <cmath>
#include <iostream>
#include <string>

using namespace qjsb;

// ── C++ class we want to expose to JS ────────────────────────────────────────

struct Vec2 {
    double x, y;

    Vec2(double x, double y) : x(x), y(y) {}

    double length()     const { return std::sqrt(x * x + y * y); }
    Vec2   add(const Vec2& o) const { return Vec2(x + o.x, y + o.y); }
    Vec2   scale(double s)    const { return Vec2(x * s, y * s); }

    std::string toString() const {
        return "Vec2(" + std::to_string(x) + ", " + std::to_string(y) + ")";
    }

    static Vec2 zero()   { return Vec2(0, 0); }
    static Vec2 unitX()  { return Vec2(1, 0); }
};

int main() {
    Runtime rt;
    Context ctx(rt);

    // Register Vec2 with the JS engine
    ClassDef<Vec2>(ctx, "Vec2")
        .constructor<double, double>()
        .field("x", &Vec2::x)
        .field("y", &Vec2::y)
        .readonlyProp("length", &Vec2::length)
        .method("add",      &Vec2::add)
        .method("scale",    &Vec2::scale)
        .method("toString", &Vec2::toString)
        .staticMethod("zero",  Vec2::zero)
        .staticMethod("unitX", Vec2::unitX)
        .endClass();

    // Bind a print helper
    ctx.bindFunction("print", [](std::string s) {
        std::cout << s << '\n';
    });

    // Use Vec2 purely from JavaScript
    ctx.eval(R"(
        var a = new Vec2(3, 4);
        print("a = " + a.toString());
        print("length of a = " + a.length);

        var b = Vec2.unitX().scale(5);
        print("b = " + b.toString());

        var c = a.add(b);
        print("a + b = " + c.toString());

        // Mutate field directly
        a.x = 0;
        print("a after x=0: " + a.toString());
    )");

    // Create a Vec2 in C++ and pass it into JS
    auto* v = new Vec2(10.0, 0.0);
    ctx.setGlobal("cppVec", pushOwned(ctx, v));   // JS now owns v

    ctx.eval(R"(
        print("cppVec = " + cppVec.toString());
        cppVec.y = 10;
        print("cppVec after y=10: length = " + cppVec.length);
    )");

    return 0;
}
