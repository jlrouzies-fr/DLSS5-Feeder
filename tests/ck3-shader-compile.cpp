// Compile the actual feeder effect with ReShade 6.8 Vulkan code generation.
// Tooling setup: UPSTREAM-REVIEW-2026-10-02.md. No game or runtime DLLs are loaded.
#include "effect_parser.hpp"
#include "effect_codegen.hpp"
#include "effect_preprocessor.hpp"
#include <iostream>
#include <fstream>
#include <memory>
int main() {
    for (int provider = 0; provider <= 4; ++provider) {
        for (int mode = 0; mode <= 1; ++mode) {
            reshadefx::preprocessor pp;
            pp.add_include_path("build/compat-tests");
            pp.add_macro_definition("__RESHADE__", "60800");
            pp.add_macro_definition("__RENDERER__", "0x20000");
            pp.add_macro_definition("__RESHADE_PERFORMANCE_MODE__", std::to_string(mode));
            pp.add_macro_definition("BUFFER_WIDTH", mode ? "3840" : "1920");
            pp.add_macro_definition("BUFFER_HEIGHT", mode ? "2160" : "1080");
            pp.add_macro_definition("BUFFER_RCP_WIDTH", "(1.0 / BUFFER_WIDTH)");
            pp.add_macro_definition("BUFFER_RCP_HEIGHT", "(1.0 / BUFFER_HEIGHT)");
            pp.add_macro_definition("BUFFER_COLOR_BIT_DEPTH", mode ? "10" : "8");
            pp.add_macro_definition("RESHADE_DEPTH_INPUT_IS_REVERSED", std::to_string(mode));
            pp.add_macro_definition("DLSS5_MV_PROVIDER", std::to_string(provider));
            if (!pp.append_file("shaders/DLSS5_Feed.fx")) { std::cerr << pp.errors(); return 1; }
            std::unique_ptr<reshadefx::codegen> backend(reshadefx::create_codegen_spirv(true, true, mode == 1));
            reshadefx::parser parser;
            if (!parser.parse(pp.output(), backend.get())) { std::cerr << pp.errors() << parser.errors(); return 1; }
            size_t bytes = 0;
            for (const auto &entry : backend->module().entry_points) {
                std::string code, assembly, errors;
                if (!backend->assemble_code_for_entry_point(entry.first, code, assembly, errors) || code.empty()) {
                    std::cerr << entry.first << ": " << errors; return 2;
                }
                const std::string path = "build/compat-tests/feeder-p" + std::to_string(provider) + "-m" + std::to_string(mode) + "-" + entry.first + ".spv";
                std::ofstream(path, std::ios::binary).write(code.data(), code.size());
                bytes += code.size();
            }
            std::cout << "PASS provider=" << provider << " performance/depth=" << mode << " entry points=" << backend->module().entry_points.size() << " SPIR-V bytes=" << bytes << '\n';
        }
    }
}
