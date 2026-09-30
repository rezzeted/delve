// Delve D1.4: F11 geometric checks over the filled level.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "check.h"
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
    EXPECT_TRUE(delve::load_project(std::string(DELVE_TEST_DATA) + "/d1_project.json", p, err))
        << err;
    p.dir = std::filesystem::path(DELVE_ASSETS_DIR).parent_path().string();
    return p;
}

delve::IrV1 buildIr(const delve::Project& p, const std::string& frozen) {
    delve::IrV1 ir;
    std::string err;
    EXPECT_TRUE(delve::build_ir_v1(readFile(frozen), frozen, p, "test", ir, err)) << err;
    return ir;
}

delve::FillResult fillIr(const delve::IrV1& ir, const delve::Project& p) {
    delve::FillOpts opts;
    opts.delve_assets = DELVE_ASSETS_DIR;
    delve::FillResult out;
    std::string err;
    EXPECT_TRUE(delve::fill_level(ir, p, opts, out, err)) << err;
    return out;
}

std::string diagText(const std::vector<delve::CheckDiag>& ds) {
    std::string t;
    for (const auto& d : ds) t += "[" + d.check + "] " + d.message + "\n";
    return t;
}

bool hasDiag(const std::vector<delve::CheckDiag>& ds, const std::string& check,
             const std::string& substr) {
    for (const auto& d : ds)
        if (d.check == check && d.message.find(substr) != std::string::npos) return true;
    return false;
}

TEST(DelveCheck, PassFrozen) {
    delve::Project p = loadD1Project();
    const delve::IrV1 ir = buildIr(p, std::string(DELVE_D0_DIR) + "/frozen_ir.json");
    const delve::FillResult fill = fillIr(ir, p);
    std::vector<delve::CheckDiag> ds;
    EXPECT_TRUE(delve::check_level(ir, p, fill, ds)) << diagText(ds);
}

TEST(DelveCheck, PassCorner) {
    delve::Project p = loadD1Project();
    const delve::IrV1 ir = buildIr(p, std::string(DELVE_TEST_DATA) + "/corner_frozen.json");
    ASSERT_EQ(ir.rooms.size(), 2u);
    ASSERT_EQ(ir.doors.size(), 1u);
    const delve::FillResult fill = fillIr(ir, p);
    std::vector<delve::CheckDiag> ds;
    EXPECT_TRUE(delve::check_level(ir, p, fill, ds)) << diagText(ds);
}

TEST(DelveCheck, PassageRejects) {
    delve::Project p = loadD1Project();
    const delve::IrV1 ir = buildIr(p, std::string(DELVE_TEST_DATA) + "/corner_frozen.json");
    ASSERT_FALSE(ir.corridor_clear.empty());
    ASSERT_FALSE(ir.doors.empty());
    {
        delve::Project bad = p;
        bad.fill.min_passage = 99.0;
        std::vector<delve::CheckDiag> ds;
        EXPECT_FALSE(delve::check_passage(ir, bad, ds));
        EXPECT_TRUE(hasDiag(ds, "passage", "min_passage")) << diagText(ds);
    }
    {
        delve::Project bad = p;
        bad.fill.min_opening = 99.0;
        std::vector<delve::CheckDiag> ds;
        EXPECT_FALSE(delve::check_passage(ir, bad, ds));
        EXPECT_TRUE(hasDiag(ds, "passage", "min_opening")) << diagText(ds);
    }
}

TEST(DelveCheck, VoidsReject) {
    delve::Project p = loadD1Project();
    p.asset_roots = {"src/tests/data", "assets"};
    p.slots["wall_body"] = "fill/body_nocuts_v1.pgg";
    const delve::IrV1 ir = buildIr(p, std::string(DELVE_TEST_DATA) + "/corner_frozen.json");
    const delve::FillResult fill = fillIr(ir, p);
    std::vector<delve::CheckDiag> ds;
    EXPECT_FALSE(delve::check_opening_voids(ir, fill, ds));
    EXPECT_TRUE(hasDiag(ds, "opening_voids", "inside opening void")) << diagText(ds);
}

TEST(DelveCheck, TransitionsReject) {
    // Facing path: wall-placed zones put both sides on one facing, so the
    // no-zones variant paints B territory with A.
    {
        delve::Project p = loadD1Project();
        p.fill.transitions.place = "wall";
        p.asset_roots = {"src/tests/data", "assets"};
        p.slots["facing"] = "fill/facing_nozones_v1.pgg";
        const delve::IrV1 ir = buildIr(p, std::string(DELVE_TEST_DATA) + "/corner_frozen.json");
        size_t zoned = 0;
        for (const auto& f : ir.facings) zoned += f.zones.size();
        ASSERT_GT(zoned, 0u) << "wall-placed corner IR must zone facings";
        const delve::FillResult fill = fillIr(ir, p);
        std::vector<delve::CheckDiag> ds;
        EXPECT_FALSE(delve::check_transitions(ir, p, fill, ds));
        EXPECT_TRUE(hasDiag(ds, "transitions", "firmly in")) << diagText(ds);
    }
    // Node path: corner zones straddle T-faces, so the no-zones variant
    // paints one side wrong there.
    {
        delve::Project p = loadD1Project();
        p.asset_roots = {"src/tests/data", "assets"};
        p.slots["node"] = "fill/node_nozones_v1.pgg";
        const delve::IrV1 ir = buildIr(p, std::string(DELVE_TEST_DATA) + "/corner_frozen.json");
        size_t zoned = 0;
        for (const auto& n : ir.nodes)
            for (const auto& fc : n.faces) zoned += fc.zones.size();
        ASSERT_GT(zoned, 0u) << "corner IR must zone node faces for this test";
        const delve::FillResult fill = fillIr(ir, p);
        std::vector<delve::CheckDiag> ds;
        EXPECT_FALSE(delve::check_transitions(ir, p, fill, ds));
        EXPECT_TRUE(hasDiag(ds, "transitions", "firmly in")) << diagText(ds);
    }
    // Chase path: the zigzag boundary must be evaluated per course, not just
    // at mid-width (either signal counts here).
    {
        delve::Project p = loadD1Project();
        p.fill.transitions.pattern = "chase";
        p.fill.transitions.place = "wall";
        p.asset_roots = {"src/tests/data", "assets"};
        p.slots["facing"] = "fill/facing_nozones_v1.pgg";
        const delve::IrV1 ir = buildIr(p, std::string(DELVE_TEST_DATA) + "/corner_frozen.json");
        const delve::FillResult fill = fillIr(ir, p);
        std::vector<delve::CheckDiag> ds;
        EXPECT_FALSE(delve::check_transitions(ir, p, fill, ds));
        EXPECT_TRUE(hasDiag(ds, "transitions", "zone")) << diagText(ds);
    }
}

TEST(DelveCheck, SpansReject) {
    delve::Project p = loadD1Project();
    delve::IrV1 ir = buildIr(p, std::string(DELVE_D0_DIR) + "/frozen_ir.json");
    ASSERT_GT(ir.walls.size(), 0u);
    ir.walls.push_back(ir.walls[0]);  // duplicated atom
    std::vector<delve::CheckDiag> ds;
    EXPECT_FALSE(delve::check_spans(ir, p, ds));
    EXPECT_TRUE(hasDiag(ds, "spans", "overlaps")) << diagText(ds);
}

}  // namespace
