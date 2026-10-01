// Delve export tests (D4, F7): frozen D0 IR -> project -> fill -> export_level,
// then the artifacts are checked structurally: OBJ vertex colors/faces,
// pgg-points/1 anchors with kind/label, delve-units/1 spans covering the mesh,
// delve-ir/3 next to them, per-unit OBJs on split_groups.

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "export.h"
#include "fill.h"
#include "ir.h"
#include "project.h"

namespace {

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

delve::Project loadD1Project() {
    delve::Project p;
    std::string err;
    const std::string path = std::string(DELVE_TEST_DATA) + "/d1_project.json";
    EXPECT_TRUE(delve::load_project(path, p, err)) << err;
    p.dir = std::filesystem::path(DELVE_ASSETS_DIR).parent_path().string();
    return p;
}

delve::IrV2 buildD1Ir(const delve::Project& p) {
    delve::IrV2 ir;
    std::string err;
    const std::string path = std::string(DELVE_D0_DIR) + "/frozen_ir.json";
    EXPECT_TRUE(delve::build_ir_v2(readFile(path), path, p, "test", ir, err)) << err;
    return ir;
}

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("delve_export_test_" + std::to_string(::getpid()));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() { std::filesystem::remove_all(path); }
    std::string str() const { return path.string(); }
};

// Counts OBJ lines of a kind ("v ", "vn ", "f "); for "v " also counts the
// colored variant (7 fields: v x y z r g b).
void objStats(const std::string& text, size_t& v, size_t& vColored, size_t& vn, size_t& f) {
    v = vColored = vn = f = 0;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("v ", 0) == 0) {
            ++v;
            std::istringstream fields(line);
            size_t n = 0;
            for (std::string tok; fields >> tok;) ++n;
            if (n == 7) ++vColored;
        } else if (line.rfind("vn ", 0) == 0) {
            ++vn;
        } else if (line.rfind("f ", 0) == 0) {
            ++f;
        }
    }
}

// One frozen fill serves both exports (Debug PGG runs are slow on purpose).
TEST(DelveExport, ExportFrozenLevel) {
    const delve::Project p = loadD1Project();
    const delve::IrV2 ir = buildD1Ir(p);
    delve::FillOpts fillOpts;
    fillOpts.delve_assets = DELVE_ASSETS_DIR;
    delve::FillResult fill;
    std::string err;
    ASSERT_TRUE(delve::fill_level(ir, p, fillOpts, fill, err)) << err;
    ASSERT_NE(fill.mesh, nullptr);
    ASSERT_NE(fill.anchors, nullptr);

    TempDir tmp;

    // Base export: obj + anchors + units + ir.
    delve::ExportOpts opts;
    opts.dir = tmp.str();
    opts.name = "level";
    delve::ExportResult res;
    ASSERT_TRUE(delve::export_level(ir, fill, opts, res, err)) << err;
    ASSERT_EQ(res.written.size(), 4u);
    for (const std::string& w : res.written)
        EXPECT_TRUE(std::filesystem::is_regular_file(w)) << w;

    // OBJ: geometry with vertex colors (@Cd) and normals. writeObj
    // triangulates the polygonal faces, so f >= faceCount (quads -> 2 tris).
    size_t v, vColored, vn, f;
    objStats(readFile(tmp.str() + "/level.obj"), v, vColored, vn, f);
    EXPECT_EQ(v, fill.mesh->pointCount());
    EXPECT_GT(vColored, 0u);
    EXPECT_GT(vn, 0u);
    EXPECT_GE(f, fill.mesh->faceCount());

    // Anchors: pgg-points/1 with kind/label columns.
    const nlohmann::json anchors = nlohmann::json::parse(readFile(tmp.str() + "/level.anchors.json"));
    EXPECT_EQ(anchors.value("format", ""), "pgg-points/1");
    ASSERT_TRUE(anchors.contains("positions"));
    EXPECT_EQ(anchors["positions"].size(), fill.anchors->pointCount());
    ASSERT_TRUE(anchors.contains("attrs"));
    EXPECT_EQ(anchors["attrs"].at("kind").at("type"), "int");
    EXPECT_EQ(anchors["attrs"].at("label").at("type"), "string");

    // Units: delve-units/1; the merge-order spans tile mesh and anchors.
    const nlohmann::json units = nlohmann::json::parse(readFile(tmp.str() + "/level.units.json"));
    EXPECT_EQ(units.value("format", ""), delve::kUnitsFormat);
    ASSERT_TRUE(units.contains("units"));
    EXPECT_EQ(units["units"].size(), fill.units.size());
    size_t meshCursor = 0, anchorsCursor = 0;
    for (const auto& u : units["units"]) {
        EXPECT_EQ(u.at("mesh_begin").get<size_t>(), meshCursor);
        meshCursor = u.at("mesh_end").get<size_t>();
        EXPECT_GT(meshCursor, u.at("mesh_begin").get<size_t>());
        EXPECT_EQ(u.at("anchors_begin").get<size_t>(), anchorsCursor);
        anchorsCursor = u.at("anchors_end").get<size_t>();
    }
    EXPECT_EQ(meshCursor, fill.mesh->pointCount());
    EXPECT_EQ(anchorsCursor, fill.anchors->pointCount());

    // IR next to the mesh: delve-ir/3, readable back (N7).
    const std::string irText = readFile(tmp.str() + "/level.ir.json");
    EXPECT_EQ(nlohmann::json::parse(irText).value("format", ""), "delve-ir/3");
    delve::IrV2 ir2;
    EXPECT_TRUE(delve::read_ir_v2_json(irText, ir2, err)) << err;

    // Split export: one extra OBJ per unit group.
    delve::ExportOpts splitOpts;
    splitOpts.dir = tmp.str();
    splitOpts.name = "split";
    splitOpts.split_groups = true;
    delve::ExportResult splitRes;
    ASSERT_TRUE(delve::export_level(ir, fill, splitOpts, splitRes, err)) << err;
    EXPECT_GT(splitRes.written.size(), 4u + fill.units.size() / 2);
    bool sawRoomObj = false;
    for (const std::string& w : splitRes.written) {
        if (w.find("split.room_") != std::string::npos) {
            sawRoomObj = true;
            EXPECT_TRUE(std::filesystem::is_regular_file(w)) << w;
        }
    }
    EXPECT_TRUE(sawRoomObj);
}

}  // namespace
