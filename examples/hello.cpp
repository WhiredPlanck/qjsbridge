// examples/hello.cpp – minimal "Hello, World" example
//
// Demonstrates:
//   • Creating a Runtime and Context
//   • Binding a C++ function to JavaScript
//   • Evaluating JS code that calls the C++ function

#include <qjsbridge.hpp>
#include <iostream>
#include <string>

using namespace qjsb;

int main() {
    Runtime rt;
    Context ctx(rt);

    // Bind a C++ lambda as a global JS function
    ctx.bindFunction("print", [](std::string s) {
        std::cout << s << '\n';
    });

    ctx.bindFunction("add", [](int a, int b) { return a + b; });

    // Evaluate JS: call the C++ functions
    ctx.eval(R"(
        print("Hello from JavaScript!");
        var result = add(21, 21);
        print("21 + 21 = " + result);
    )");

    // Read a JS value back into C++
    Value v = ctx.eval("add(100, 23)");
    int32_t n = 0;
    JS_ToInt32(ctx.get(), &n, v.get());
    std::cout << "add(100, 23) = " << n << '\n';

    return 0;
}
