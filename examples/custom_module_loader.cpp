// examples/custom_module_loader.cpp – custom module loader with in-memory sources

#include <qjsbridge.hpp>
#include <filesystem>
#include <iostream>
#include <optional>

using namespace qjsb;

int main() {
    Runtime rt;
    Context ctx(rt);

    ModuleLoader loader;
    loader
        .setNormalize([](JSContext*, const std::string& base, const std::string& name) {
            if (name == "pkg") return std::string("virtual/pkg.mjs");
            if (name == "./dep") {
                const std::filesystem::path base_path(base);
                const std::filesystem::path dir =
                    base_path.has_parent_path() ? base_path.parent_path() : std::filesystem::path(".");
                return (dir / "dep.mjs").generic_string();
            }
            return name;
        })
        .setSourceLoader([](JSContext*, const std::string& normalized_name)
            -> std::optional<std::string> {
            if (normalized_name == "virtual/pkg.mjs")
                return std::string("import { n } from './dep'; export const value = n + 1;");
            if (normalized_name == "virtual/dep.mjs")
                return std::string("export const n = 41;");
            return std::nullopt;
        });

    rt.setModuleLoader(std::move(loader));

    ctx.evalModule(R"(
        import { value } from "pkg";
        globalThis.msg = `custom module loader value=${value}`;
    )", "loader_demo.mjs");

    std::cout << ctx.eval("msg").toString() << "\n";
    return 0;
}
