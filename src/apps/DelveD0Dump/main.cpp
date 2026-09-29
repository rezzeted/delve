// DelveD0Dump: one-shot frozen-IR dump for D0 (requirements §11).
// Runs edgar on a tutorial preset with a fixed seed and writes frozen_ir.json,
// rooms.points.json, d0_view.pgg and reference.png. Not part of ctest: layout
// generation is reproducible only on one platform/build (N1) — the committed
// docs/d0 artifacts are the frozen result, verified by delve_d0_test.
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>

#include "edgar/generator/grid2d/graph_based_generator_configuration.hpp"
#include "edgar/generator/grid2d/graph_based_generator_grid2d.hpp"
#include "edgar/generator/grid2d/layout_door_computation.hpp"
#include "ir_dump.h"
#include "preset_loader.hpp"

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: DelveD0Dump --resources <edgar_gui dir> --map <name.yml> --out <dir> "
                 "[--seed N] [--cell M]\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string resources;
    std::string map_name = "tutorial_corridors.yml";
    std::string out_dir;
    int seed = 1;
    double cell = 2.0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need_value = [&](std::string& out) {
            if (i + 1 >= argc) {
                usage();
                std::exit(2);
            }
            out = argv[++i];
        };
        if (arg == "--resources") {
            need_value(resources);
        } else if (arg == "--map") {
            need_value(map_name);
        } else if (arg == "--out") {
            need_value(out_dir);
        } else if (arg == "--seed") {
            std::string v;
            need_value(v);
            seed = std::atoi(v.c_str());
        } else if (arg == "--cell") {
            std::string v;
            need_value(v);
            cell = std::strtod(v.c_str(), nullptr);
        } else {
            usage();
            return 2;
        }
    }
    if (resources.empty() || out_dir.empty() || !(cell > 0.0)) {
        usage();
        return 2;
    }

    namespace grid2d = edgar::generator::grid2d;
    auto loaded = grid2d::load_preset_catalog_with_status(resources);
    if (!loaded.error.empty()) {
        std::fprintf(stderr, "catalog: %s\n", loaded.error.c_str());
        return 1;
    }
    const grid2d::PresetMap* map = nullptr;
    for (const auto& m : loaded.catalog.maps) {
        if (m.filename == map_name) {
            map = &m;
            break;
        }
    }
    if (!map) {
        std::fprintf(stderr, "map not found: %s\n", map_name.c_str());
        return 1;
    }
    grid2d::LevelDescriptionGrid2D<int> level = grid2d::build_level_from_preset(*map, loaded.catalog);
    grid2d::GraphBasedGeneratorConfiguration config{};
    grid2d::GraphBasedGeneratorGrid2D<int> generator(level, config);
    generator.inject_random_generator(std::mt19937(static_cast<unsigned>(seed)));
    grid2d::LayoutGrid2D<int> layout = generator.generate_layout();
    // Doors are a separate pass (same as the level-synth app); the door rng is
    // derived from the seed so the dump stays deterministic (N1, one platform).
    std::mt19937 door_rng(static_cast<unsigned>(seed));
    grid2d::compute_layout_doors(layout, level, level.get_graph(), door_rng);
    std::printf("layout: %zu rooms\n", layout.rooms.size());

    delve::d0::DumpConfig cfg;
    cfg.map_name = map_name;
    cfg.seed = seed;
    cfg.cell = cell;
    std::string err;
    if (!delve::d0::dump_frozen_ir(layout, cfg, out_dir, err)) {
        std::fprintf(stderr, "dump: %s\n", err.c_str());
        return 1;
    }
    std::printf("dumped frozen IR to %s\n", out_dir.c_str());
    return 0;
}
