// Delve D1.3: R-A3 slot checks (lib assets pass, broken assets fail with
// delve/slot errors naming slot/asset/param/expectation/fact) + F6 fill.

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
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
    // Lamp ids are room-local (D3): deco:lamp:<room>:<k>, one anchor each.
    size_t lampUnits = 0;
    for (const auto& [unit, n] : perUnit)
        if (unit.rfind("deco:lamp:", 0) == 0) {
            ++lampUnits;
            EXPECT_EQ(n, 1) << unit;
        }
    EXPECT_EQ(lampUnits, out.stats.lamps);
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

// --- F8 unit cache -----------------------------------------------------------

bool loadText(const std::string& text, delve::Project& p, std::string& err) {
    const std::string path =
        (std::filesystem::path(testing::TempDir()) / "d1_fill_probe.json").string();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    out.close();
    return delve::load_project(path, p, err);
}

std::string surgery(const std::string& text, const std::string& from, const std::string& to) {
    const size_t first = text.find(from);
    EXPECT_NE(first, std::string::npos) << "anchor missing: " << from;
    EXPECT_EQ(text.find(from, first + 1), std::string::npos) << "anchor ambiguous: " << from;
    std::string out = text;
    out.replace(first, from.size(), to);
    return out;
}

delve::FillResult fillCached(const delve::IrV2& ir, const delve::Project& p,
                             delve::UnitCache& cache) {
    delve::FillOpts opts;
    opts.delve_assets = DELVE_ASSETS_DIR;
    opts.cache = &cache;
    delve::FillResult out;
    std::string err;
    EXPECT_TRUE(delve::fill_level(ir, p, opts, out, err)) << err;
    return out;
}

std::set<std::string> asSet(const std::vector<std::string>& v) {
    return {v.begin(), v.end()};
}

TEST(DelveFill, UnitKeyStableAndSensitive) {
    const std::string assets = DELVE_ASSETS_DIR;
    uint64_t ak = 0;
    std::string err;
    ASSERT_TRUE(delve::asset_content_key(assets + "/walls/facing_v1.pgg", {assets}, ak, err))
        << err;
    const std::vector<std::pair<std::string, pgg::Value>> bindings = {
        {"h", pgg::Value(2.6f)}, {"style", pgg::Value(2)}, {"rng_seed", pgg::Value(42)}};
    delve::UnitKey k1, k2;
    ASSERT_TRUE(delve::unit_key("facing", ak, bindings, k1, err)) << err;
    ASSERT_TRUE(delve::unit_key("facing", ak, bindings, k2, err)) << err;
    EXPECT_EQ(k1, k2) << "same inputs must key equal";

    auto rekey = [&](std::pair<std::string, pgg::Value> one, size_t at) {
        std::vector<std::pair<std::string, pgg::Value>> b = bindings;
        b[at].second = std::move(one.second);
        delve::UnitKey k;
        EXPECT_TRUE(delve::unit_key("facing", ak, b, k, err)) << err;
        return k;
    };
    EXPECT_NE(k1, rekey({"", pgg::Value(2.6000001f)}, 0)) << "float bits must matter";
    EXPECT_NE(k1, rekey({"", pgg::Value(3)}, 1)) << "int value must matter";
    EXPECT_NE(k1, rekey({"", pgg::Value(43)}, 2)) << "rng_seed must matter";
    delve::UnitKey otherSlot, otherAsset;
    ASSERT_TRUE(delve::unit_key("node", ak, bindings, otherSlot, err)) << err;
    EXPECT_NE(k1, otherSlot) << "slot kind must matter";
    ASSERT_TRUE(delve::unit_key("facing", ak ^ 1, bindings, otherAsset, err)) << err;
    EXPECT_NE(k1, otherAsset) << "asset content must matter";
}

TEST(DelveFill, AssetContentKeyCoversImports) {
    const std::string tmp = testing::TempDir() + "/delve_cache_key";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    std::filesystem::copy(DELVE_ASSETS_DIR, tmp, std::filesystem::copy_options::recursive);
    const std::string facing = tmp + "/walls/facing_v1.pgg";
    std::string err;
    uint64_t base = 0, touched = 0;
    ASSERT_TRUE(delve::asset_content_key(facing, {tmp}, base, err)) << err;
    {
        std::ofstream f(facing, std::ios::app);
        f << "# touch\n";
    }
    ASSERT_TRUE(delve::asset_content_key(facing, {tmp}, touched, err)) << err;
    EXPECT_NE(base, touched) << "asset bytes must matter";
    base = touched;
    {
        std::ofstream f(tmp + "/patterns.pgg", std::ios::app);
        f << "# touch\n";
    }
    ASSERT_TRUE(delve::asset_content_key(facing, {tmp}, touched, err)) << err;
    EXPECT_NE(base, touched) << "import closure bytes must matter (R-A7)";
}

TEST(DelveFill, CacheTransparent) {
    delve::Project p = loadD1Project();
    const delve::IrV2 ir = buildD1Ir(p);
    delve::UnitCache cache;
    const delve::FillResult warm = fillCached(ir, p, cache);
    EXPECT_EQ(warm.stats.reran.size(), warm.units.size());
    EXPECT_TRUE(warm.stats.reused.empty());
    const delve::FillResult hot = fillCached(ir, p, cache);
    EXPECT_EQ(hot.stats.reused.size(), hot.units.size());
    EXPECT_TRUE(hot.stats.reran.empty());
    // A cache hit must be invisible in the output (byte-level).
    ASSERT_EQ(warm.mesh->pointCount(), hot.mesh->pointCount());
    ASSERT_EQ(warm.anchors->pointCount(), hot.anchors->pointCount());
    EXPECT_EQ(*warm.mesh->positions, *hot.mesh->positions);
    EXPECT_EQ(*warm.anchors->positions, *hot.anchors->positions);
}

// Editing fill parameters must recompute exactly the units whose input
// changed (F8 refill at a frozen IR; cheap empty assets, the assertion is on
// the recomputed id set, not on geometry). The frozen level is hall-only, so
// the edits are: "*" height (invalidates heights everywhere) and door_h
// (invalidates doors, door-cut walls and door-cut facings only).
TEST(DelveFill, CacheInvalidatesOnParamEdit) {
    const std::string base = readFile(std::string(DELVE_TEST_DATA) + "/d1_project.json");
    delve::Project p;
    std::string err;
    ASSERT_TRUE(loadText(base, p, err)) << err;
    p.dir = std::filesystem::path(DELVE_ASSETS_DIR).parent_path().string();
    p.asset_roots = {"src/tests/data/fill", "assets"};
    p.slots["room_fill"] = "empty_room_fill.pgg";
    p.slots["wall_body"] = "empty_wall_body.pgg";
    p.slots["facing"] = "empty_facing.pgg";
    p.slots["node"] = "empty_node.pgg";
    p.slots["door"] = "empty_door.pgg";
    p.slots["decor:lamp"] = "empty_decor.pgg";
    const delve::IrV2 ir0 = buildD1Ir(p);

    delve::UnitCache cache;
    fillCached(ir0, p, cache);  // warm

    auto editProject = [&](const std::string& text, delve::Project& out) {
        ASSERT_TRUE(loadText(text, out, err)) << err;
        out.dir = p.dir;
        out.asset_roots = p.asset_roots;
        out.slots = p.slots;
    };

    // Edit 1: "*" height 3.0 -> 3.1. Rooms, walls, facings and nodes bind
    // heights; doors bind door_h and lamps bind no height: reused.
    const std::string text1 = surgery(base, "\"*\": {\"h\": 3.0", "\"*\": {\"h\": 3.1");
    delve::Project p1;
    editProject(text1, p1);
    const delve::IrV2 ir1 = buildD1Ir(p1);
    const delve::FillResult refill1 = fillCached(ir1, p1, cache);
    {
        std::set<std::string> want;
        for (const auto& r : ir1.rooms) want.insert("room:" + r.id);
        for (const auto& w : ir1.walls) want.insert(w.id);
        for (const auto& f : ir1.facings) want.insert(f.id);
        for (const auto& n : ir1.nodes) want.insert(n.id);
        EXPECT_EQ(asSet(refill1.stats.reran), want);
        for (const std::string& id : refill1.stats.reused)
            EXPECT_TRUE(id.rfind("door:", 0) == 0 || id.rfind("deco:", 0) == 0) << id;
    }

    // Edit 2 (on top of edit 1): door_h 2.2 -> 2.3. Only doors, walls with
    // door cuts and facings with door cuts bind it.
    delve::Project p2;
    editProject(surgery(text1, "\"door_h\": 2.2", "\"door_h\": 2.3"), p2);
    const delve::IrV2 ir2 = buildD1Ir(p2);
    const delve::FillResult refill2 = fillCached(ir2, p2, cache);
    {
        std::set<std::string> want;
        for (const auto& d : ir2.doors) want.insert(d.id);
        for (const auto& w : ir2.walls)
            if (!w.doors.empty()) want.insert(w.id);
        for (const auto& f : ir2.facings)
            if (!f.cuts.empty()) want.insert(f.id);
        EXPECT_EQ(asSet(refill2.stats.reran), want);
        EXPECT_EQ(refill2.stats.reran.size() + refill2.stats.reused.size(),
                  refill2.units.size());
    }

    delve::UnitCache cold;
    const delve::FillResult fresh = fillCached(ir2, p2, cold);
    EXPECT_EQ(*refill2.mesh->positions, *fresh.mesh->positions);
}

// Editing an asset recomputes only its slot's units; editing an import
// recomputes every dependent slot (R-A7). Real facing/node assets so the
// import-closure assertions are meaningful.
TEST(DelveFill, CacheInvalidatesOnAssetEdit) {
    const std::string tmp = testing::TempDir() + "/delve_cache_assets";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    std::filesystem::copy(DELVE_ASSETS_DIR, tmp + "/assets",
                          std::filesystem::copy_options::recursive);
    const std::string data = std::string(DELVE_TEST_DATA) + "/fill";
    for (const char* f : {"empty_room_fill.pgg", "empty_wall_body.pgg", "empty_door.pgg",
                          "empty_decor.pgg"})
        std::filesystem::copy_file(data + "/" + f, tmp + "/assets/" + f);

    delve::Project p = loadD1Project();
    p.dir = tmp;
    p.slots["room_fill"] = "empty_room_fill.pgg";
    p.slots["wall_body"] = "empty_wall_body.pgg";
    p.slots["door"] = "empty_door.pgg";
    p.slots["decor:lamp"] = "empty_decor.pgg";
    const delve::IrV2 ir = buildD1Ir(p);
    std::set<std::string> facIds, nodeIds;
    for (const auto& f : ir.facings) facIds.insert(f.id);
    for (const auto& n : ir.nodes) nodeIds.insert(n.id);

    delve::UnitCache cache;
    fillCached(ir, p, cache);  // warm

    std::string err;
    {
        std::ofstream f(tmp + "/assets/walls/facing_v1.pgg", std::ios::app);
        f << "# touch\n";
    }
    const delve::FillResult afterAsset = fillCached(ir, p, cache);
    EXPECT_EQ(asSet(afterAsset.stats.reran), facIds) << "asset edit must hit only its slot";

    {
        std::ofstream f(tmp + "/assets/patterns.pgg", std::ios::app);
        f << "# touch\n";
    }
    const delve::FillResult afterImport = fillCached(ir, p, cache);
    std::set<std::string> want = facIds;
    want.insert(nodeIds.begin(), nodeIds.end());
    EXPECT_EQ(asSet(afterImport.stats.reran), want) << "import edit must hit dependents (R-A7)";

    delve::UnitCache cold;
    const delve::FillResult fresh = fillCached(ir, p, cold);
    EXPECT_EQ(*afterImport.mesh->positions, *fresh.mesh->positions);
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
