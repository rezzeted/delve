// Delve D1.3: R-A3 slot checks (lib assets pass, broken assets fail with
// delve/slot errors naming slot/asset/param/expectation/fact) + F6 fill.

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pgg/eval.h"
#include "pgg/src/eval/geometry.h"
#include "pgg/src/eval/param_text.h"
#include "fill.h"
#include "ir.h"
#include "project.h"

namespace {

bool hasDiag(const std::vector<delve::SlotDiag>& ds, const std::string& code,
             const std::string& substr) {
    for (const auto& d : ds)
        if (d.code == code && d.message.find(substr) != std::string::npos) return true;
    return false;
}

std::string diagText(const std::vector<delve::SlotDiag>& ds) {
    std::string t;
    for (const auto& d : ds) t += "[" + d.code + "] " + d.message + "\n";
    return t;
}

TEST(DelveFill, Ra3LibraryPasses) {
    const std::string assets = DELVE_ASSETS_DIR;
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"room_fill", "rooms/fill_v1.pgg"},
        {"wall_body", "walls/body_v1.pgg"},
        {"facing", "walls/facing_v1.pgg"},
        {"node", "walls/node_v1.pgg"},
        {"door", "doors/opening_v1.pgg"},
        {"decor:lamp", "decor/lamp_v1.pgg"},
    };
    for (const auto& [slot, asset] : cases) {
        SCOPED_TRACE(slot + " " + asset);
        std::vector<delve::SlotDiag> ds;
        EXPECT_TRUE(delve::check_asset(slot, assets + "/" + asset, {assets}, ds))
            << diagText(ds);
    }
    // Plain "decor" normalizes like "decor:<tag>".
    std::vector<delve::SlotDiag> ds;
    EXPECT_TRUE(delve::check_asset("decor", assets + "/decor/lamp_v1.pgg", {assets}, ds))
        << diagText(ds);
}

TEST(DelveFill, Ra3Rejects) {
    const std::string dir = std::string(DELVE_TEST_DATA) + "/fill";
    struct Case {
        const char* file;
        const char* code;
        const char* substr;
    };
    const std::vector<Case> cases = {
        {"ra3_missing_param.pgg", "delve/slot", "missing param 'zones'"},
        {"ra3_wrong_type.pgg", "delve/slot", "param 'module': expected f32, got int"},
        {"ra3_missing_output.pgg", "delve/slot", "missing output 'anchors'"},
        {"ra3_extra_nodefault.pgg", "delve/slot", "extra param 'tint' needs a default"},
        {"ra3_no_version.pgg", "delve/slot", "def slot_version() is missing"},
        {"ra3_bad_version.pgg", "delve/slot", "slot_version() is 2, required 1"},
        {"ra3_version_body.pgg", "delve/slot", "must be exactly"},
        {"ra3_scalar_nodefault.pgg", "delve/slot", "param 'h' needs a default"},
        {"ra3_static_error.pgg", "E103", "bogus_undefined"},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.file);
        std::vector<delve::SlotDiag> ds;
        EXPECT_FALSE(delve::check_asset("facing", dir + "/" + c.file, {}, ds));
        EXPECT_TRUE(hasDiag(ds, c.code, c.substr)) << diagText(ds);
    }
    // Unknown slot kind (no file involved).
    {
        std::vector<delve::SlotDiag> ds;
        EXPECT_FALSE(delve::check_asset("bogus", dir + "/ra3_missing_param.pgg", {}, ds));
        EXPECT_TRUE(hasDiag(ds, "delve/slot", "unknown slot kind")) << diagText(ds);
    }
}

TEST(DelveFill, Ra3AcceptsEdges) {
    const std::string dir = std::string(DELVE_TEST_DATA) + "/fill";
    for (const char* f : {"ra3_extra_default_ok.pgg", "ra3_version_docstring.pgg"}) {
        SCOPED_TRACE(f);
        std::vector<delve::SlotDiag> ds;
        EXPECT_TRUE(delve::check_asset("facing", dir + "/" + f, {}, ds)) << diagText(ds);
    }
}

// --- F6 --------------------------------------------------------------------

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
    // slots resolve against the committed asset library here.
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

const std::vector<int64_t>* intCol(const pgg::GeoPtr& g, const char* name) {
    if (!g->pointAttrs) return nullptr;
    const pgg::AttrColumn* c = g->pointAttrs->find(name);
    if (!c) return nullptr;
    auto* v = std::get_if<std::shared_ptr<const std::vector<int64_t>>>(&c->data);
    return v ? v->get() : nullptr;
}

const std::vector<std::string>* strCol(const pgg::GeoPtr& g, const char* name) {
    if (!g->pointAttrs) return nullptr;
    const pgg::AttrColumn* c = g->pointAttrs->find(name);
    if (!c) return nullptr;
    auto* v = std::get_if<std::shared_ptr<const std::vector<std::string>>>(&c->data);
    return v ? v->get() : nullptr;
}

const std::vector<glm::vec3>* vec3Col(const pgg::GeoPtr& g, const char* name) {
    if (!g->pointAttrs) return nullptr;
    const pgg::AttrColumn* c = g->pointAttrs->find(name);
    if (!c) return nullptr;
    auto* v = std::get_if<std::shared_ptr<const std::vector<glm::vec3>>>(&c->data);
    return v ? v->get() : nullptr;
}

TEST(DelveFill, FillFrozenLevel) {
    delve::Project p = loadD1Project();
    const delve::IrV2 ir = buildD1Ir(p);
    delve::FillOpts opts;
    opts.delve_assets = DELVE_ASSETS_DIR;
    delve::FillResult out;
    std::string err;
    ASSERT_TRUE(delve::fill_level(ir, p, opts, out, err)) << err;
    EXPECT_EQ(out.stats.rooms, 17u);
    EXPECT_GT(out.stats.bodies, 0u);
    EXPECT_GT(out.stats.facings, 0u);
    EXPECT_GT(out.stats.nodes, 0u);
    EXPECT_GT(out.stats.doors, 0u);
    EXPECT_GT(out.stats.lamps, 0u);
    ASSERT_NE(out.mesh, nullptr);
    ASSERT_NE(out.anchors, nullptr);
    EXPECT_EQ(out.mesh->kind, pgg::GeoKind::Mesh);
    EXPECT_GT(out.mesh->pointCount(), 0u);
    EXPECT_EQ(out.anchors->kind, pgg::GeoKind::Points);

    // Exact anchors: rooms contribute light (+spawn for halls), lamps one each.
    size_t halls = 0;
    for (const auto& r : ir.rooms)
        if (r.role == "hall") ++halls;
    EXPECT_EQ(out.anchors->pointCount(), ir.rooms.size() + halls + out.stats.lamps);

    // Labels: "<unit_id>#<kind>", kinds consistent, only room/lamp units emit.
    const auto* kinds = intCol(out.anchors, "kind");
    const auto* labels = strCol(out.anchors, "label");
    ASSERT_NE(kinds, nullptr);
    ASSERT_NE(labels, nullptr);
    ASSERT_EQ(kinds->size(), out.anchors->pointCount());
    std::map<std::string, int> perUnit;
    for (size_t i = 0; i < labels->size(); ++i) {
        const std::string want =
            (*kinds)[i] == 1 ? "light" : ((*kinds)[i] == 2 ? "spawn" : "poi");
        EXPECT_EQ((*labels)[i].substr((*labels)[i].size() - want.size() - 1), "#" + want)
            << (*labels)[i];
        const std::string unit = (*labels)[i].substr(0, (*labels)[i].size() - want.size() - 1);
        EXPECT_TRUE(unit.rfind("room:", 0) == 0 || unit.rfind("deco:lamp:", 0) == 0) << unit;
        if (unit.rfind("room:", 0) == 0)
            EXPECT_TRUE((*kinds)[i] == 1 || (*kinds)[i] == 2) << (*labels)[i];
        else
            EXPECT_EQ((*kinds)[i], 1) << (*labels)[i];
        ++perUnit[unit];
    }
    for (const auto& r : ir.rooms) {
        const int n = perUnit["room:" + r.id];
        EXPECT_EQ(n, r.role == "hall" ? 2 : 1) << r.id;
    }
    for (size_t k = 0; k < out.stats.lamps; ++k)
        EXPECT_EQ(perUnit["deco:lamp:" + std::to_string(k)], 1) << k;
}

TEST(DelveFill, FillDeterministic) {
    delve::Project p = loadD1Project();
    const delve::IrV2 ir = buildD1Ir(p);
    delve::FillOpts opts;
    opts.delve_assets = DELVE_ASSETS_DIR;
    delve::FillResult a, b;
    std::string err;
    ASSERT_TRUE(delve::fill_level(ir, p, opts, a, err)) << err;
    ASSERT_TRUE(delve::fill_level(ir, p, opts, b, err)) << err;
    ASSERT_EQ(a.mesh->pointCount(), b.mesh->pointCount());
    ASSERT_EQ(a.anchors->pointCount(), b.anchors->pointCount());
    for (size_t i = 0; i < a.mesh->pointCount(); ++i) {
        EXPECT_EQ((*a.mesh->positions)[i], (*b.mesh->positions)[i]) << i;
        if (i > 16) break;  // spot-check head; full sweep below counts on bytes
    }
    EXPECT_EQ(*a.mesh->positions, *b.mesh->positions);
    EXPECT_EQ(*a.anchors->positions, *b.anchors->positions);
}

// A rotated facing (wall along Z) filled through F6 (local frame + assembly)
// must match the same asset run directly in the world frame.
TEST(DelveFill, RotatedFrameEquivalence) {
    delve::Project p = loadD1Project();
    const delve::IrV2 ir = buildD1Ir(p);
    // Prefer a vertical facing hosting a transition zone.
    const delve::IrFacing* target = nullptr;
    for (const auto& f : ir.facings) {
        const double dx = f.to.first - f.from.first, dz = f.to.second - f.from.second;
        if (std::abs(dz) > std::abs(dx) && !f.zones.empty()) {
            target = &f;
            break;
        }
    }
    if (!target)
        for (const auto& f : ir.facings) {
            const double dx = f.to.first - f.from.first, dz = f.to.second - f.from.second;
            if (std::abs(dz) > std::abs(dx)) {
                target = &f;
                break;
            }
        }
    ASSERT_NE(target, nullptr) << "frozen IR has no vertical facing";

    // Isolate the facing: every other slot gets an empty asset.
    const std::string data = DELVE_TEST_DATA;
    p.asset_roots = {"src/tests/data/fill", "assets"};
    p.slots["room_fill"] = "empty_room_fill.pgg";
    p.slots["wall_body"] = "empty_wall_body.pgg";
    p.slots["node"] = "empty_node.pgg";
    p.slots["door"] = "empty_door.pgg";
    p.slots["decor:lamp"] = "empty_decor.pgg";
    delve::FillOpts opts;
    opts.delve_assets = DELVE_ASSETS_DIR;
    // Keep only the target facing (drop everything else from a scratch IR).
    delve::IrV2 one;
    one.rooms = ir.rooms;  // rooms expand but run empty assets
    one.facings = {*target};
    delve::FillResult filled;
    std::string err;
    ASSERT_TRUE(delve::fill_level(one, p, opts, filled, err)) << err;
    ASSERT_GT(filled.mesh->pointCount(), 0u);

    // Direct world-frame run of the same unit (same seed).
    const double dx = target->to.first - target->from.first;
    const double dz = target->to.second - target->from.second;
    const double len = std::hypot(dx, dz);
    const double ux = dx / len, uz = dz / len;
    const std::string tmp = testing::TempDir() + "/delve_rot_frame";
    std::filesystem::create_directories(tmp);
    {
        std::ostringstream seg;
        seg << "{\"format\": \"pgg-points/1\", \"positions\": [[" << target->from.first
            << ", 0, " << target->from.second << "], [" << target->to.first << ", 0, "
            << target->to.second << "]]}";
        std::ofstream(tmp + "/seg.points.json") << seg.str();
        std::ostringstream cuts;
        cuts << "{\"format\": \"pgg-points/1\", \"positions\": [";
        std::string hs;
        for (size_t i = 0; i < target->cuts.size(); ++i) {
            if (i) {
                cuts << ", ";
                hs += ", ";
            }
            cuts << "[" << target->cuts[i].a.first << ", 0, " << target->cuts[i].a.second
                 << "], [" << target->cuts[i].b.first << ", 0, " << target->cuts[i].b.second
                 << "]";
            hs += std::to_string(target->cuts[i].h) + ", " + std::to_string(target->cuts[i].h);
        }
        cuts << "], \"attrs\": {\"h\": {\"type\": \"f32\", \"values\": [" << hs << "]}}}";
        std::ofstream(tmp + "/cuts.points.json") << cuts.str();
        auto ints = [&](const char* name, auto get) {
            std::ostringstream s;
            s << "\"" << name << "\": {\"type\": \"int\", \"values\": [";
            for (size_t i = 0; i < target->zones.size(); ++i) {
                if (i) s << ", ";
                s << get(target->zones[i]) << ", " << get(target->zones[i]);
            }
            s << "]}";
            return s.str();
        };
        auto flts = [&](const char* name, auto get) {
            std::ostringstream s;
            s << "\"" << name << "\": {\"type\": \"f32\", \"values\": [";
            for (size_t i = 0; i < target->zones.size(); ++i) {
                if (i) s << ", ";
                s << get(target->zones[i]) << ", " << get(target->zones[i]);
            }
            s << "]}";
            return s.str();
        };
        std::ostringstream zones;
        zones << "{\"format\": \"pgg-points/1\", \"positions\": [";
        for (size_t i = 0; i < target->zones.size(); ++i) {
            if (i) zones << ", ";
            const auto& z = target->zones[i];
            zones << "[" << target->from.first + ux * z.l0 << ", 0, "
                  << target->from.second + uz * z.l0 << "], [" << target->from.first + ux * z.l1
                  << ", 0, " << target->from.second + uz * z.l1 << "]";
        }
        zones << "], \"attrs\": {" << ints("zone", [](const delve::ZonePiece& z) {
            return z.zone;
        }) << ", " << ints("pattern", [](const delve::ZonePiece& z) { return z.pattern; })
              << ", " << ints("seed", [](const delve::ZonePiece& z) { return z.seed; })
              << ", " << flts("t_at_l0", [](const delve::ZonePiece& z) { return z.t_at_l0; })
              << ", " << ints("flip", [](const delve::ZonePiece& z) { return z.flip; })
              << ", " << flts("width", [](const delve::ZonePiece& z) { return z.width; })
              << ", " << flts("module", [](const delve::ZonePiece& z) { return z.module; })
              << ", " << ints("style_a", [](const delve::ZonePiece& z) { return z.style_a; })
              << ", " << ints("style_b", [](const delve::ZonePiece& z) { return z.style_b; })
              << "}}";
        std::ofstream(tmp + "/zones.points.json") << zones.str();
    }
    bool ok = false;
    const int style = delve::style_code(target->style, ok);
    ASSERT_TRUE(ok);
    pgg::RunParams rp;
    rp.importRoots = {std::string(DELVE_ASSETS_DIR)};
    const std::vector<std::pair<std::string, std::string>> params = {
        {"seg", "@seg.points.json"},
        {"n", "(" + std::to_string(target->n.first) + ", 0, " +
                  std::to_string(target->n.second) + ")"},
        {"h", std::to_string(target->h)},
        {"style", std::to_string(style)},
        {"module", std::to_string(p.fill.row_module)},
        {"cuts", "@cuts.points.json"},
        {"zones", "@zones.points.json"},
        {"rng_seed", std::to_string(delve::unit_seed(p.seed, target->id))},
    };
    for (const auto& [name, text] : params) {
        pgg::Value v;
        ASSERT_TRUE(pgg::parseParamText(text, tmp, v, &err)) << err;
        rp.values.emplace_back(name, v);
    }
    const pgg::RunResult r =
        pgg::runFile(std::string(DELVE_ASSETS_DIR) + "/walls/facing_v1.pgg", rp);
    ASSERT_FALSE(r.hasErrors());
    pgg::GeoPtr expect;
    for (const auto& o : r.outputs)
        if (o.name == "mesh") expect = pgg::asGeo(o.value);
    ASSERT_NE(expect, nullptr);
    ASSERT_EQ(filled.mesh->pointCount(), expect->pointCount());
    double worst = 0;
    for (size_t i = 0; i < expect->pointCount(); ++i) {
        const glm::vec3 d = (*filled.mesh->positions)[i] - (*expect->positions)[i];
        worst = std::max(worst, (double)std::max({std::abs(d.x), std::abs(d.y), std::abs(d.z)}));
    }
    EXPECT_LT(worst, 1e-4) << "local+assembly vs world-frame divergence";
    // Same paint (transition zones resolved identically).
    EXPECT_EQ(*intCol(filled.mesh, "style"), *intCol(expect, "style"));
}

TEST(DelveFill, AssetVariants) {
    const std::string assets = DELVE_ASSETS_DIR;
    std::string err;
    // Gate doors carry bars: strictly more geometry than the open leaf.
    auto runDoor = [&](int dtype) {
        pgg::RunParams rp;
        rp.importRoots = {assets};
        const std::string dir = assets + "/doors";
        const std::vector<std::pair<std::string, std::string>> params = {
            {"seg", "@opening_v1.seg.points.json"}, {"h", "2.2"}, {"frame", "0.15"},
            {"thick", "0.6"}, {"dtype", std::to_string(dtype)}, {"rng_seed", "7"}};
        for (const auto& [name, text] : params) {
            pgg::Value v;
            EXPECT_TRUE(pgg::parseParamText(text, dir, v, &err)) << err;
            rp.values.emplace_back(name, v);
        }
        const pgg::RunResult r = pgg::runFile(dir + "/opening_v1.pgg", rp);
        EXPECT_FALSE(r.hasErrors());
        for (const auto& o : r.outputs)
            if (o.name == "mesh") return pgg::asGeo(o.value)->pointCount();
        return size_t(0);
    };
    EXPECT_LT(runDoor(1), runDoor(2));

    // Wall-mounted lamps aim along the mount normal (mount-agnostic asset).
    const std::string tmp = testing::TempDir() + "/delve_wall_lamp";
    std::filesystem::create_directories(tmp);
    std::ofstream(tmp + "/p.points.json")
        << "{\"format\": \"pgg-points/1\", \"positions\": [[3, 1.5, 0]], "
           "\"attrs\": {\"n\": {\"type\": \"vec3\", \"values\": [[1, 0, 0]]}}}";
    pgg::RunParams rp;
    rp.importRoots = {assets};
    for (const auto& [name, text] :
         std::vector<std::pair<std::string, std::string>>{
             {"p", "@p.points.json"}, {"style", "1"}, {"tag", "1"}, {"rng_seed", "7"}}) {
        pgg::Value v;
        ASSERT_TRUE(pgg::parseParamText(text, tmp, v, &err)) << err;
        rp.values.emplace_back(name, v);
    }
    const pgg::RunResult r = pgg::runFile(assets + "/decor/lamp_v1.pgg", rp);
    ASSERT_FALSE(r.hasErrors());
    pgg::GeoPtr anch;
    for (const auto& o : r.outputs)
        if (o.name == "anchors") anch = pgg::asGeo(o.value);
    ASSERT_NE(anch, nullptr);
    ASSERT_EQ(anch->pointCount(), 1u);
    const auto* dir = vec3Col(anch, "dir");
    ASSERT_NE(dir, nullptr);
    EXPECT_EQ((*dir)[0], glm::vec3(1, 0, 0));
    const glm::vec3 dp = (*anch->positions)[0] - glm::vec3(3.3f, 1.5f, 0.0f);
    EXPECT_LT(std::abs(dp.x) + std::abs(dp.y) + std::abs(dp.z), 1e-6f);
}

TEST(DelveFill, FillRejects) {
    delve::Project p = loadD1Project();
    const delve::IrV2 ir = buildD1Ir(p);
    delve::FillOpts opts;
    opts.delve_assets = DELVE_ASSETS_DIR;
    delve::FillResult out;
    std::string err;
    // Missing slot entry for a needed kind.
    {
        delve::Project bad = p;
        bad.slots.erase("door");
        ASSERT_GT(ir.doors.size(), 0u);
        EXPECT_FALSE(delve::fill_level(ir, bad, opts, out, err));
        EXPECT_NE(err.find("project has no 'door'"), std::string::npos) << err;
    }
    // Broken asset surfaces the R-A3 error.
    {
        delve::Project bad = p;
        bad.dir = DELVE_TEST_DATA;
        bad.asset_roots = {".", "../../../assets"};
        bad.slots["facing"] = "fill/ra3_missing_param.pgg";
        EXPECT_FALSE(delve::fill_level(ir, bad, opts, out, err));
        EXPECT_NE(err.find("missing param 'zones'"), std::string::npos) << err;
    }
}

}  // namespace
