// examples/module_binding.cpp – export C++ bindings as an ES module

#include <qjsbridge.hpp>
#include <iostream>

using namespace qjsb;

struct Point {
    double x, y;
    Point(double x, double y) : x(x), y(y) {}
    double length2() const { return x * x + y * y; }
};

int main() {
    Runtime rt;
    Context ctx(rt);

    auto module = ctx.newModule("geom");
    module.bindFunction("add", [](int a, int b) { return a + b; });

    ClassDef<Point>(ctx, "Point")
        .constructor<double, double>()
        .field("x", &Point::x)
        .field("y", &Point::y)
        .method("length2", &Point::length2)
        .endClass(module);

    ctx.evalModule(R"(
        import { add, Point } from "geom";
        globalThis.msg = `add(2,3)=${add(2,3)}, len2=${new Point(3,4).length2()}`;
    )", "module_demo.mjs");

    std::cout << ctx.eval("msg").toString() << "\n";
    return 0;
}

