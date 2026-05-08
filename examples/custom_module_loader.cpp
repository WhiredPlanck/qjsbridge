// examples/custom_module_loader.cpp – custom module loader with in-memory sources

#include <qjsbridge.hpp>
#include <iostream>
#include <string_view>
#include <unordered_map>

using namespace qjsb;

int main() {
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

    ctx.moduleLoader = [&files](std::string_view filename) -> ModuleData {
        auto it = files.find(std::string(filename));
        if (it != files.end())
            return ModuleData{detail::toUri(filename), it->second};
        return {};
    };
    ctx.bindFunction("log", [](std::string_view msg) {
        std::cout << msg << "\n";
    });

    ctx.evalModule(R"(
        import "./some_module.js";
    )", "<eval>");
    return 0;
}
