// examples/containers.cpp – converting STL containers between C++ and JS
//
// Demonstrates:
//   • std::vector<T>  ↔  JS Array
//   • std::map<K,V>   ↔  JS Object
//   • std::optional<T> ↔  JS null / value
//   • std::set<T>      ↔  JS Array (unordered on the JS side)

#include <qjsbridge.hpp>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace qjsb;

int main() {
    Runtime rt;
    Context ctx(rt);

    ctx.bindFunction("print", [](std::string s) { std::cout << s << '\n'; });

    // ── vector ────────────────────────────────────────────────────────────────
    ctx.bindFunction("sumInts", [](std::vector<int> v) {
        int s = 0; for (int x : v) s += x; return s;
    });
    ctx.bindFunction("range", [](int n) {
        std::vector<int> v; for (int i = 0; i < n; ++i) v.push_back(i); return v;
    });

    ctx.eval(R"(
        print("sum([1,2,3,4,5]) = " + sumInts([1, 2, 3, 4, 5]));
        print("range(5) = " + JSON.stringify(range(5)));
    )");

    // ── map ───────────────────────────────────────────────────────────────────
    ctx.bindFunction("wordLengths", [](std::vector<std::string> words) {
        std::map<std::string, int> m;
        for (const auto& w : words) m[w] = static_cast<int>(w.size());
        return m;
    });

    ctx.eval(R"(
        var wl = wordLengths(["hello", "world", "qjsbridge"]);
        print("wordLengths: " + JSON.stringify(wl));
    )");

    // ── optional ──────────────────────────────────────────────────────────────
    ctx.bindFunction("divide", [](double a, double b) -> std::optional<double> {
        if (b == 0.0) return std::nullopt;
        return a / b;
    });

    ctx.eval(R"(
        var r1 = divide(10, 3);
        var r2 = divide(5, 0);
        print("10/3 = " + r1);
        print("5/0 = " + r2);   // null
    )");

    // ── set (round-trip via JSON) ─────────────────────────────────────────────
    ctx.bindFunction("deduplicate", [](std::vector<int> v) {
        std::set<int> s(v.begin(), v.end());
        return std::vector<int>(s.begin(), s.end());
    });

    ctx.eval(R"(
        var deduped = deduplicate([3, 1, 4, 1, 5, 9, 2, 6, 5, 3]);
        print("deduped (sorted): " + JSON.stringify(deduped));
    )");

    // ── nested containers ─────────────────────────────────────────────────────
    ctx.bindFunction("transpose", [](std::vector<std::vector<int>> m) {
        if (m.empty() || m[0].empty()) return std::vector<std::vector<int>>{};
        std::size_t rows = m.size(), cols = m[0].size();
        std::vector<std::vector<int>> t(cols, std::vector<int>(rows));
        for (std::size_t r = 0; r < rows; ++r)
            for (std::size_t c = 0; c < cols; ++c)
                t[c][r] = m[r][c];
        return t;
    });

    ctx.eval(R"(
        var mat   = [[1,2,3],[4,5,6]];
        var trans = transpose(mat);
        print("transpose([[1,2,3],[4,5,6]]) = " + JSON.stringify(trans));
    )");

    return 0;
}
