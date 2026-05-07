# qjsbridge

A lightweight, header-only C++17 binding library that bridges
[QuickJS](https://bellard.org/quickjs/) (and QuickJS-ng) to C++.

## Features

- **Header-only, zero external dependencies** (only QuickJS itself)
- **RAII wrappers** – `Runtime`, `Context`, `Value` with correct lifetime
  and ownership semantics
- **Automatic type conversion** via `Converter<T>` specialisations
- **Function binding** – free functions, lambdas, capturing closures
- **Module binding** – export C++ functions/classes as ES modules and import via
  `import { ... } from "module"`
- **Class binding** – constructors, member methods, data fields, static
  methods, read-only properties, custom getter/setter pairs
- **Ownership models** – owned (`T*`), borrowed (`const T*`), and
  `std::shared_ptr<T>` hand-off
- **Containers** – `vector`, `map`, `unordered_map`, `set`,
  `unordered_set`, `array`, `optional` (with recursive nesting)
- **Enums** – bidirectional mapping through the underlying integer type
- **C++ ↔ JS exception bridging** – JS exceptions propagate as
  `qjsb::JSException`; C++ exceptions thrown inside callbacks are
  re-raised as JS errors

## Requirements

| Requirement       | Detail                              |
|-------------------|-------------------------------------|
| C++ standard      | C++17 or later                      |
| QuickJS           | Original QuickJS **or** QuickJS-ng  |
| Build system      | CMake ≥ 3.16 (or include manually)  |

## Quick Start

### CMake integration (FetchContent)

```cmake
include(FetchContent)
FetchContent_Declare(qjsbridge
    GIT_REPOSITORY https://github.com/WhiredPlanck/qjsbridge.git
    GIT_TAG        main)
FetchContent_MakeAvailable(qjsbridge)

target_link_libraries(my_app PRIVATE qjsbridge::qjsbridge quickjs)
```

### Single-header include

```cpp
#include <qjsbridge.hpp>   // umbrella header
```

For QuickJS-ng define `QJSBRIDGE_QUICKJS_NG` before including:

```cpp
#define QJSBRIDGE_QUICKJS_NG
#include <qjsbridge.hpp>
```

---

## Hello World

```cpp
#include <qjsbridge.hpp>
#include <iostream>

int main() {
    qjsb::Runtime rt;
    qjsb::Context ctx(rt);

    ctx.bindFunction("print", [](std::string s) {
        std::cout << s << '\n';
    });
    ctx.bindFunction("add", [](int a, int b) { return a + b; });

    ctx.eval(R"(
        print("Hello from JavaScript!");
        print("1 + 2 = " + add(1, 2));
    )");
}
```

### Module export / import

```cpp
auto mod = ctx.newModule("math_ext");
mod.bindFunction("add", [](int a, int b) { return a + b; });

ctx.evalModule(R"(
  import { add } from "math_ext";
  globalThis.result = add(20, 22);
)", "entry.mjs");
```

---

## Type Conversions

| C++ type                              | JS type          |
|---------------------------------------|------------------|
| `bool`                                | `boolean`        |
| `int8_t` … `int64_t`, `uint8_t` … `uint32_t` | `number` |
| `float`, `double`                     | `number`         |
| scoped/unscoped `enum`                | `number`         |
| `std::string`, `std::string_view`     | `string`         |
| `const char*`                         | `string` / null  |
| `std::vector<T>`                      | `Array`          |
| `std::array<T, N>`                    | `Array`          |
| `std::map<K,V>`, `std::unordered_map<K,V>` | `Object`  |
| `std::set<T>`, `std::unordered_set<T>` | `Array`         |
| `std::optional<T>`                    | `T` / `null`     |
| `T` (registered class, by value)      | JS object (owned)|
| `T*` (registered class)               | JS object (owned)|
| `const T*`                            | JS object (borrowed)|
| `std::shared_ptr<T>`                  | JS object (shared)|
| `T&`, `const T&`                      | JS object        |
| `qjsb::Value`, `JSValue`              | passthrough      |

### Custom converters

Specialise `qjsb::Converter<MyType>`:

```cpp
template <>
struct qjsb::Converter<MyType> {
    static JSValue    to_js(JSContext* ctx, MyType v)      { /* ... */ }
    static MyType     from_js(JSContext* ctx, JSValueConst v) { /* ... */ }
    static bool       accept(JSContext* ctx, JSValueConst v)  { /* optional */ }
};
```

---

## Function Binding

```cpp
// Free function
int  add(int a, int b) { return a + b; }
ctx.bindFunction("add", add);

// Lambda
ctx.bindFunction("mul", [](double a, double b) { return a * b; });

// Capturing lambda
int offset = 100;
ctx.bindFunction("shift", [offset](int x) { return x + offset; });

// Manual placement
qjsb::Value fn = qjsb::makeFunction(ctx, [](int x) { return x * x; });
ctx.setGlobal("square", std::move(fn));
```

---

## Class Binding

```cpp
struct Vec2 {
    double x, y;
    Vec2(double x, double y) : x(x), y(y) {}
    double length() const { return std::sqrt(x*x + y*y); }
    Vec2   add(const Vec2& o) const { return {x+o.x, y+o.y}; }
    static Vec2 zero() { return {0, 0}; }
};

qjsb::ClassDef<Vec2>(ctx, "Vec2")
    .constructor<double, double>()   // new Vec2(x, y)
    .field("x", &Vec2::x)            // direct field access (get + set)
    .field("y", &Vec2::y)
    .property("length", &Vec2::length)      // getter-only overload
    .method("add",  &Vec2::add)
    .staticMethod("zero", Vec2::zero)
    .endClass();                     // registers Vec2 as a global

// From JavaScript:
//   var v = new Vec2(3, 4);
//   console.log(v.length);   // 5
//   var w = Vec2.zero();
//   var u = v.add(w);
```

### Ownership semantics

```cpp
// JS takes ownership (deletes on GC)
ctx.setGlobal("v", qjsb::pushOwned(ctx, new Vec2(1, 0)));

// JS borrows: C++ object must outlive the JS value
Vec2 local(0, 1);
ctx.setGlobal("v", qjsb::pushBorrowed(ctx, &local));

// Shared ownership
auto sp = std::make_shared<Vec2>(3, 4);
ctx.setGlobal("v", qjsb::pushShared(ctx, sp));
```

---

## Error Handling

```cpp
try {
    ctx.eval("throw new Error('oops')");
} catch (const qjsb::JSException& e) {
    std::cerr << e.what() << '\n';   // includes stack trace
    std::cerr << e.stack() << '\n';
}

// C++ exceptions thrown inside callbacks are re-raised as JS errors:
ctx.bindFunction("boom", []() -> int {
    throw std::runtime_error("C++ error");
    return 0;
});
// In JS: try { boom(); } catch(e) { /* e.message == "C++ error" */ }
```

---

## API Reference

### `qjsb::Runtime`
RAII owner of `JSRuntime`. Constructor throws `qjsb::Exception` on failure.

| Method              | Description                         |
|---------------------|-------------------------------------|
| `get()`             | Raw `JSRuntime*`                    |
| `setMemoryLimit(n)` | Limit JS heap size                  |
| `setMaxStackSize(n)`| Limit JS stack                      |
| `runGC()`           | Trigger garbage collection          |
| `setModuleLoader(normalize, loader, opaque)` | Install custom QuickJS module loader |

### `qjsb::Context`
RAII owner of `JSContext`.

| Method                             | Description                              |
|------------------------------------|------------------------------------------|
| `eval(code, filename?, flags?)`    | Evaluate JS string; throws on error      |
| `evalFile(path)`                   | Read and evaluate a file                 |
| `globalObject()`                   | Returns the global `Value`               |
| `getGlobal(name)` / `setGlobal(name, v)` | Access global variables          |
| `newObject()` / `newArray()`       | Create JS objects/arrays                 |
| `bindFunction(name, fn, length?)`  | Bind a C++ callable as a global function |
| `newModule(name)`                  | Create a C module for ES `import` exports |
| `evalModule(code, filename?)`      | Evaluate module code (`JS_EVAL_TYPE_MODULE`) |
| `get()`                            | Raw `JSContext*`                         |

### `qjsb::Value`
Owning RAII wrapper for `JSValue`.

| Member                          | Description                               |
|---------------------------------|-------------------------------------------|
| `get()`, `ctx()`                | Raw accessors                             |
| `steal()`                       | Release ownership (caller frees)          |
| `Value::dup(ctx, v)`            | Create a reference-counted copy           |
| `isUndefined()`, `isNull()`, `isBool()`, `isNumber()`, `isString()`, `isObject()`, `isArray()`, `isFunction()` | Type predicates |
| `toString()`                    | JS toString conversion                    |
| `operator[](name)` / `operator[](idx)` | Property/index access              |
| `set(name/idx, value)`          | Property/index write                      |
| `length()`                      | Array/string length                       |
| `call(...)` / `callAsConstructor(...)` | Invoke as function/constructor      |

### `qjsb::ClassDef<T>`

| Method                              | Description                               |
|-------------------------------------|-------------------------------------------|
| `constructor<Args...>()`            | Register constructor with given argument types |
| `method(name, fn)`                  | Instance method (member ptr or free fn)   |
| `field(name, member_ptr)`           | Direct field with auto getter+setter      |
| `property(name, getter)`            | Getter-only property                       |
| `property(name, getter, setter)`    | Read-write property                        |
| `staticMethod(name, fn)`            | Static method on the constructor          |
| `endClass(global_name?)`            | Publish constructor as global variable    |
| `endClass(module, export_name?)`    | Export constructor from a module          |
| `constructorValue()`                | Returns constructor `Value` for manual placement |

### `qjsb::Module`

| Method                                 | Description                              |
|----------------------------------------|------------------------------------------|
| `bindFunction(name, fn, length?)`      | Export callable from module              |
| `exportValue(name, value)`             | Export existing JS value                 |
| `get()`                                | Raw `JSModuleDef*`                       |

### Free helpers

```cpp
qjsb::makeFunction(ctx, fn, name?, length?)  // callable → Value
qjsb::pushOwned(ctx, T*)                     // C++ → JS (JS owns)
qjsb::pushBorrowed(ctx, T*)                  // C++ → JS (borrowed)
qjsb::pushShared(ctx, shared_ptr<T>)         // C++ → JS (shared)
qjsb::getPtr<T>(ctx, jsval)                  // JS → T*
qjsb::to_js(ctx, value)                      // Converter<T>::to_js
qjsb::from_js<T>(ctx, jsval)                 // Converter<T>::from_js
```

---

## Building

```bash
cmake -B build
cmake --build build
ctest --test-dir build
```

If QuickJS is installed to a non-standard prefix:

```bash
cmake -B build -DQUICKJS_INCLUDE_DIRS=/path/to/include \
               -DQUICKJS_LIBRARIES=/path/to/libquickjs.a
```

---

## Limitations (v0.1)

- **Single-threaded**: no thread-safety guarantees
- **uint64_t**: values above 2⁵³ lose precision (JS only has `number`)
- **Class value semantics**: returning a class by value copies it to the
  heap – prefer returning `T*` or `std::shared_ptr<T>` for large objects
- **No overload resolution**: binding two functions with the same JS name
  overwrites the previous binding
- **QuickJS-ng**: define `QJSBRIDGE_QUICKJS_NG` when using QuickJS-ng
  (changes `JS_NewClassID` call signature)

---

## License

MIT
// Export class constructor from a module instead of global:
auto geom = ctx.newModule("geom");
qjsb::ClassDef<Vec2>(ctx, "Vec2")
    .constructor<double, double>()
    .property("length", &Vec2::length)
    .endClass(geom);                 // export { Vec2 } from "geom"
