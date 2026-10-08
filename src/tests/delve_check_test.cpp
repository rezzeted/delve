// Delve D1.4-D1.5: F11 geometric checks over the filled level + transition
// paint (butt/chase x corner/wall on the stone|brick corner scene).

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "check.h"
#include "fill.h"
#include "ir.h"
#include "project.h"

#include "catalog.h"
#include "generate.h"

#include "pgg/src/eval/geometry.h"
#include "pgg/src/eval/param_text.h"

namespace {

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

// Replace the unique `from` substring; fails the test when absent/ambiguous.
std::string surgery(const std::string& text, const std::string& from, const std::string& to) {
    const size_t first = text.find(from);
    EXPECT_NE(first, std::string::npos) << "anchor missing: " << from;
    EXPECT_EQ(text.find(from, first + 1), std::string::npos) << "anchor ambiguous: " << from;
    std::string out = text;
    out.replace(first, from.size(), to);
    return out;
}

bool loadText(const std::string& text, delve::Project& p, std::string& err) {
    const std::string path =
        (std::filesystem::path(testing::TempDir()) / "d2_check_probe.json").string();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    out.close();
    return delve::load_project(path, p, err);
}

delve::Project loadD1Project() {
    delve::Project p;
    std::string err;
    EXPECT_TRUE(delve::load_project(std::string(DELVE_TEST_DATA) + "/d1_project.json", p, err))
        << err;
    p.dir = std::filesystem::path(DELVE_ASSETS_DIR).parent_path().string();
    return p;
}

delve::IrV2 buildIr(const delve::Project& p, const std::string& frozen) {
    delve::IrV2 ir;
    std::string err;
    EXPECT_TRUE(delve::build_ir_v2(readFile(frozen), frozen, p, "test", ir, err)) << err;
    return ir;
}

delve::FillResult fillIr(const delve::IrV2& ir, const delve::Project& p) {
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
    const delve::IrV2 ir = buildIr(p, std::string(DELVE_D0_DIR) + "/frozen_ir.json");
    const delve::FillResult fill = fillIr(ir, p);
    std::vector<delve::CheckDiag> ds;
    EXPECT_TRUE(delve::check_level(ir, p, fill, ds)) << diagText(ds);
}

TEST(DelveCheck, PassCorner) {
    delve::Project p = loadD1Project();
    const delve::IrV2 ir = buildIr(p, std::string(DELVE_TEST_DATA) + "/corner_frozen.json");
    ASSERT_EQ(ir.rooms.size(), 2u);
    ASSERT_EQ(ir.doors.size(), 1u);
    const delve::FillResult fill = fillIr(ir, p);
    std::vector<delve::CheckDiag> ds;
    EXPECT_TRUE(delve::check_level(ir, p, fill, ds)) << diagText(ds);
}

TEST(DelveCheck, PassageRejects) {
    delve::Project p = loadD1Project();
    const delve::IrV2 ir = buildIr(p, std::string(DELVE_TEST_DATA) + "/corner_frozen.json");
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
    const delve::IrV2 ir = buildIr(p, std::string(DELVE_TEST_DATA) + "/corner_frozen.json");
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
        const delve::IrV2 ir = buildIr(p, std::string(DELVE_TEST_DATA) + "/corner_frozen.json");
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
        const delve::IrV2 ir = buildIr(p, std::string(DELVE_TEST_DATA) + "/corner_frozen.json");
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
        const delve::IrV2 ir = buildIr(p, std::string(DELVE_TEST_DATA) + "/corner_frozen.json");
        const delve::FillResult fill = fillIr(ir, p);
        std::vector<delve::CheckDiag> ds;
        EXPECT_FALSE(delve::check_transitions(ir, p, fill, ds));
        EXPECT_TRUE(hasDiag(ds, "transitions", "zone")) << diagText(ds);
    }
}

TEST(DelveCheck, SpansReject) {
    delve::Project p = loadD1Project();
    delve::IrV2 ir = buildIr(p, std::string(DELVE_D0_DIR) + "/frozen_ir.json");
    ASSERT_GT(ir.walls.size(), 0u);
    ir.walls.push_back(ir.walls[0]);  // duplicated atom
    std::vector<delve::CheckDiag> ds;
    EXPECT_FALSE(delve::check_spans(ir, p, ds));
    EXPECT_TRUE(hasDiag(ds, "spans", "overlaps")) << diagText(ds);
}

// D1.5: butt/chase x corner/wall on the stone|brick corner scene. check_level
// must pass, and every firmly-inside seam face must carry the pattern's paint
// (mirror of patterns.pgg zone_side; margins sit just inside the element
// half-length, sound by the whole-element rule: kept elements never cross a
// paint boundary).
namespace corner5 {

double chaseBoundary(double y, double width, double module) {
    const long course = (long)std::floor(y / module);
    return width * 0.5 + ((course % 2 == 0) ? -module * 0.5 : module * 0.5);
}

const delve::FillResult::UnitSpan* findSpan(const delve::FillResult& fill,
                                            const std::string& id) {
    for (const auto& s : fill.units)
        if (s.id == id) return &s;
    return nullptr;
}

size_t checkPaint(const delve::IrV2& ir, const delve::FillResult& fill,
                   const std::string& tag) {
    if (!fill.mesh->pointAttrs) {
        ADD_FAILURE() << tag;
        return 0;
    }
    const pgg::AttrColumn* c = fill.mesh->pointAttrs->find("style");
    if (!c) {
        ADD_FAILURE() << tag;
        return 0;
    }
    const auto* v = std::get_if<std::shared_ptr<const std::vector<int64_t>>>(&c->data);
    if (!v) {
        ADD_FAILURE() << tag;
        return 0;
    }
    const std::vector<int64_t>& styles = **v;
    const auto& corners = *fill.mesh->cornerVerts;
    const auto& offs = *fill.mesh->faceOffsets;
    const auto& pos = *fill.mesh->positions;
    size_t asserted = 0, skipped_mixed = 0;
    for (const auto& f : ir.facings) {
        if (f.zones.empty()) continue;
        const delve::FillResult::UnitSpan* span = findSpan(fill, f.id);
        if (!span) {
            ADD_FAILURE() << tag << " " << f.id;
            return 0;
        }
        const double seg_len = std::hypot(f.to.first - f.from.first, f.to.second - f.from.second);
        const double ux = (f.to.first - f.from.first) / seg_len;
        const double uz = (f.to.second - f.from.second) / seg_len;
        struct CutL { double lo, hi, h; };
        std::vector<CutL> cuts;
        for (const auto& ct : f.cuts) {
            const double la =
                (ct.a.first - f.from.first) * ux + (ct.a.second - f.from.second) * uz;
            const double lb =
                (ct.b.first - f.from.first) * ux + (ct.b.second - f.from.second) * uz;
            cuts.push_back({std::min(la, lb), std::max(la, lb), ct.h});
        }
        for (size_t fi = 0; fi + 1 < offs.size(); ++fi) {
            if (offs[fi + 1] == offs[fi]) continue;
            bool inside = true;
            glm::vec3 centroid(0);
            int64_t st = -1;
            bool unanimous = true;
            for (int32_t ci = offs[fi]; ci < offs[fi + 1]; ++ci) {
                const int pi = corners[ci];
                if (pi < (int)span->meshBegin || pi >= (int)span->meshEnd) {
                    inside = false;
                    break;
                }
                centroid += pos[pi];
                if (st < 0)
                    st = styles[pi];
                else if (styles[pi] != st)
                    unanimous = false;
            }
            if (!inside) continue;
            centroid /= (float)(offs[fi + 1] - offs[fi]);
            const double l =
                (centroid.x - f.from.first) * ux + (centroid.z - f.from.second) * uz;
            const double y = centroid.y;
            const delve::ZonePiece* piece = nullptr;
            for (const auto& z : f.zones)
                if (l >= z.l0 && l <= z.l1) piece = &z;  // last covering wins
            if (!piece) continue;
            if (piece->flip != 0) {
                ADD_FAILURE() << tag << " " << f.id << ": facing pieces run with +s";
                continue;
            }
            // Margin just INSIDE the element half-length (module): the zone is
            // one element wide per side, so anything larger filters everything
            // out. Sound by existence: a kept element never crosses a paint
            // boundary, so an existing face this far from every boundary
            // belongs to a unanimous element (centroid float noise ~1e-7).
            const double margin = piece->module - 1e-3;
            // Piece edges are paint boundaries only where another piece of the
            // same facing abuts (overlaps are pathological, gaps are doors or
            // unit ends whose elements cannot cross). Corner-place halves meet
            // at the joint on different units: no margin there.
            bool near_abut = false;
            for (const auto& z : f.zones) {
                if (&z == piece) continue;
                if (z.l1 > piece->l0 - margin && z.l0 < piece->l0 && l < piece->l0 + margin)
                    near_abut = true;
                if (z.l0 < piece->l1 + margin && z.l1 > piece->l1 && l > piece->l1 - margin)
                    near_abut = true;
            }
            if (near_abut) continue;
            bool near_cut = false;
            for (const auto& ct : cuts) {
                if (l > ct.lo - margin && l < ct.hi + margin) near_cut = true;
                if (std::abs(y - ct.h) < piece->module) near_cut = true;
            }
            if (near_cut) continue;
            const double t = piece->t_at_l0 + l;
            if (t < margin || t > piece->width - margin) continue;  // firmly in zone
            const double b = (piece->pattern == 0)
                                 ? piece->width * 0.5
                                 : chaseBoundary(y, piece->width, piece->module);
            // The mid boundary needs a margin only when strictly inside this
            // piece's t-range; corner-place halves end AT the joint (t = w/2)
            // and are single-sided.
            const double t_lo = piece->t_at_l0 + piece->l0;
            const double t_hi = piece->t_at_l0 + piece->l1;
            if (b > t_lo + margin && b < t_hi - margin && std::abs(t - b) < margin) continue;
            if (!unanimous) {
                ++skipped_mixed;
                continue;
            }
            const int expected = (t < b) ? piece->style_a : piece->style_b;
            EXPECT_EQ(st, expected) << tag << " " << f.id << " zone " << piece->zone << " l=" << l
                                    << " t=" << t << " y=" << y;
            ++asserted;
        }
    }
    if (asserted > 0)
        EXPECT_LT(skipped_mixed, asserted) << tag << ": too many mixed-style faces";
    return asserted;
}

// Node-face paint: same zone_side mirror. Node frame is translate-only
// (origin = pillar center at base); face +x = right of the outward normal
// (slots §2.4), l = 0 at the face center; elements are thick/2 long with no
// bond. Only the dressing is asserted: pillar faces and dressing backs sit
// exactly ON the face plane (d = thick/2), fronts/caps stick out to +0.05.
size_t checkNodePaint(const delve::IrV2& ir, const delve::FillResult& fill,
                     const std::string& tag, double cell) {
    const pgg::AttrColumn* c = fill.mesh->pointAttrs->find("style");
    const auto* v = std::get_if<std::shared_ptr<const std::vector<int64_t>>>(&c->data);
    const std::vector<int64_t>& styles = **v;
    const auto& corners = *fill.mesh->cornerVerts;
    const auto& offs = *fill.mesh->faceOffsets;
    const auto& pos = *fill.mesh->positions;
    size_t asserted = 0;
    for (const auto& n : ir.nodes) {
        bool zoned = false;
        for (const auto& fc : n.faces) zoned |= !fc.zones.empty();
        if (!zoned) continue;
        const delve::FillResult::UnitSpan* span = findSpan(fill, n.id);
        if (!span) {
            ADD_FAILURE() << tag << " " << n.id;
            return 0;
        }
        const double ox = n.at.first * cell, oz = n.at.second * cell;
        const double half = n.thick * 0.5;
        const double margin = half * 0.5 - 1e-3;  // element half-length just inside
        for (size_t fi = 0; fi + 1 < offs.size(); ++fi) {
            if (offs[fi + 1] == offs[fi]) continue;
            bool inside = true;
            glm::vec3 centroid(0);
            int64_t st = -1;
            bool unanimous = true;
            for (int32_t ci = offs[fi]; ci < offs[fi + 1]; ++ci) {
                const int pi = corners[ci];
                if (pi < (int)span->meshBegin || pi >= (int)span->meshEnd) {
                    inside = false;
                    break;
                }
                centroid += pos[pi];
                if (st < 0)
                    st = styles[pi];
                else if (styles[pi] != st)
                    unanimous = false;
            }
            if (!inside || !unanimous) continue;
            centroid /= (float)(offs[fi + 1] - offs[fi]);
            for (const auto& fc : n.faces) {
                if (fc.zones.empty()) continue;
                const double d = (centroid.x - ox) * fc.n.first + (centroid.z - oz) * fc.n.second;
                if (d < half + 1e-3) continue;  // pillar + dressing backs
                const double cx = ox + fc.center.first, cz = oz + fc.center.second;
                // right of normal in (x, z): (nz, -nx), verified against the
                // asset yaw math (face_rows: yaw = atan2(nx, nz)).
                const double rx = fc.n.second, rz = -fc.n.first;
                const double l = (centroid.x - cx) * rx + (centroid.z - cz) * rz;
                const double y = centroid.y;
                const delve::ZonePiece* piece = nullptr;
                for (const auto& z : fc.zones)
                    if (l >= z.l0 && l <= z.l1) piece = &z;
                if (!piece) continue;
                const double t =
                    (piece->flip == 0) ? piece->t_at_l0 + l : piece->t_at_l0 - l;
                if (t < margin || t > piece->width - margin) continue;
                const double b = (piece->pattern == 0)
                                     ? piece->width * 0.5
                                     : chaseBoundary(y, piece->width, piece->module);
                // Piece t-range under flip (t decreasing in l when flipped).
                const double ta = (piece->flip == 0) ? piece->t_at_l0 + piece->l0
                                                     : piece->t_at_l0 - piece->l1;
                const double tb = (piece->flip == 0) ? piece->t_at_l0 + piece->l1
                                                     : piece->t_at_l0 - piece->l0;
                if (b > ta + margin && b < tb - margin && std::abs(t - b) < margin) continue;
                const int expected = (t < b) ? piece->style_a : piece->style_b;
                EXPECT_EQ(st, expected) << tag << " " << n.id << " l=" << l << " t=" << t
                                        << " y=" << y << " d=" << d;
                ++asserted;
            }
        }
    }
    return asserted;
}

}  // namespace corner5

TEST(DelveCheck, TransitionPaintStoneBrick) {
    const char* patterns[2] = {"butt", "chase"};
    const char* places[2] = {"corner", "wall"};
    for (const char* pattern : patterns) {
        for (const char* place : places) {
            const std::string tag = std::string(pattern) + "+" + place;
            delve::Project p = loadD1Project();
            p.fill.transitions.pattern = pattern;
            p.fill.transitions.place = place;
            const delve::IrV2 ir =
                buildIr(p, std::string(DELVE_TEST_DATA) + "/corner_frozen.json");
            size_t stone_brick = 0;
            for (const auto& t : ir.transitions) {
                const bool ab = (t.style_a == "stone" && t.style_b == "brick") ||
                                (t.style_a == "brick" && t.style_b == "stone");
                stone_brick += ab ? 1 : 0;
            }
            ASSERT_GT(stone_brick, 0u) << tag << ": corner scene must join stone|brick";
            const delve::FillResult fill = fillIr(ir, p);
            std::vector<delve::CheckDiag> ds;
            EXPECT_TRUE(delve::check_level(ir, p, fill, ds)) << tag << "\n" << diagText(ds);
            const size_t fac = corner5::checkPaint(ir, fill, tag);
            const size_t nod = corner5::checkNodePaint(ir, fill, tag, p.fill.cell);
            EXPECT_GT(fac + nod, 0u) << tag << ": no firmly-inside seam faces found";
            if (std::string(place) == "wall")
                EXPECT_GT(fac, 0u) << tag << ": wall-placed seams must paint facings";
            else
                EXPECT_GT(nod, 0u) << tag << ": corner T-seams must paint node faces";
        }
    }
}

// D2.3b: the full chain "project -> layout -> IR -> fill -> F11" for a
// generated level (d2_project: entry -c1- hall; c1-hall passage is a gate).
// The corridor role's wall_t override is removed: with per-room thickness the
// corridor's 0.5 vs the others' 0.6 would be a 5.2 error on shared walls.
delve::Project loadD2ChainProject() {
    const std::string base = readFile(std::string(DELVE_TEST_DATA) + "/d2_project.json");
    delve::Project p;
    std::string err;
    EXPECT_TRUE(loadText(surgery(base, ", \"wall_t\": 0.5", ""), p, err)) << err;
    // slots resolve against the committed asset library here.
    p.dir = std::filesystem::path(DELVE_ASSETS_DIR).parent_path().string();
    return p;
}

delve::LayoutData generateLayoutData(delve::Project& p) {
    delve::layout::Catalog cat;
    std::string err;
    EXPECT_TRUE(delve::layout::build_catalog(p, cat, err)) << err;
    delve::layout::LayoutGenerator gen;
    delve::layout::LayoutResult lr;
    delve::layout::GenerateOptions opts;
    opts.attempts = 4;
    EXPECT_TRUE(gen.generate(p, cat, opts, lr, err)) << err;
    std::string ltext;
    EXPECT_TRUE(delve::layout::write_layout_json(lr, p, "chain", ltext, err)) << err;
    delve::LayoutData ld;
    EXPECT_TRUE(delve::read_layout_json(ltext, ld, err)) << err;
    return ld;
}

delve::IrV2 buildChainIr(const delve::LayoutData& ld, delve::Project& p) {
    delve::IrV2 ir;
    std::string err;
    EXPECT_TRUE(delve::build_ir_from_layout(ld, p, "chain", ir, err)) << err;
    return ir;
}

delve::IrV2 generateIr(delve::Project& p) { return buildChainIr(generateLayoutData(p), p); }

delve::FillResult fillChain(const delve::IrV2& ir, const delve::Project& p,
                            delve::UnitCache& cache) {
    delve::FillOpts opts;
    opts.delve_assets = DELVE_ASSETS_DIR;
    opts.cache = &cache;
    delve::FillResult out;
    std::string err;
    EXPECT_TRUE(delve::fill_level(ir, p, opts, out, err)) << err;
    return out;
}

TEST(DelveCheck, GenerateRectLevelEndToEnd) {
    delve::Project p = loadD2ChainProject();
    const delve::IrV2 ir = generateIr(p);
    ASSERT_FALSE(ir.rooms.empty());
    EXPECT_TRUE(ir.from_layout);
    EXPECT_GT(ir.transitions.size(), 0u);  // corridor sides are brick vs stone
    // The gate passage reaches the door unit's dtype (asset draws the bars).
    bool saw_gate = false;
    for (const auto& d : ir.doors) saw_gate = saw_gate || d.dtype == 2;
    EXPECT_TRUE(saw_gate);
    const delve::FillResult fill = fillIr(ir, p);
    std::vector<delve::CheckDiag> ds;
    EXPECT_TRUE(delve::check_level(ir, p, fill, ds)) << diagText(ds);
}

TEST(DelveCheck, GenerateFiguredLevelEndToEnd) {
    // rooms_rect narrows to entry; the hall must adopt the L-shaped grand_hall.
    const std::string base = readFile(std::string(DELVE_TEST_DATA) + "/d2_project.json");
    std::string text = surgery(base, ", \"wall_t\": 0.5", "");
    text = surgery(text, "\"rooms_rect\": {\"w\": [4, 5], \"h\": [4, 5]}",
                   "\"rooms_rect\": {\"w\": [4, 5], \"h\": [4, 5], \"roles\": [\"entry\"]}");
    delve::Project q;
    std::string err;
    ASSERT_TRUE(loadText(text, q, err)) << err;
    q.dir = std::filesystem::path(DELVE_ASSETS_DIR).parent_path().string();
    // The v1 room_fill asset is rect-only by contract (assets_v1.md); the
    // figured hall runs the empty test double, everything else is real.
    q.asset_roots = {"src/tests/data", "assets"};
    q.slots["room_fill"] = "fill/empty_room_fill.pgg";

    const delve::IrV2 ir = generateIr(q);
    bool saw_figured = false;
    for (const auto& r : ir.rooms) saw_figured = saw_figured || r.grid.size() > 4;
    ASSERT_TRUE(saw_figured) << "the hall must adopt the L-shaped grand_hall";
    const delve::FillResult fill = fillIr(ir, q);
    std::vector<delve::CheckDiag> ds;
    EXPECT_TRUE(delve::check_level(ir, q, fill, ds)) << diagText(ds);
}

// D2 acceptance (§11): changing the corridor width re-lays the level out and
// passability still holds (check_level includes passage/opening minimums).
TEST(DelveCheck, CorridorWidthChangeKeepsPassage) {
    {
        delve::Project p = loadD2ChainProject();
        const delve::IrV2 ir = generateIr(p);
        const delve::FillResult fill = fillIr(ir, p);
        std::vector<delve::CheckDiag> ds;
        ASSERT_TRUE(delve::check_level(ir, p, fill, ds)) << diagText(ds);
    }
    const std::string base = readFile(std::string(DELVE_TEST_DATA) + "/d2_project.json");
    std::string text = surgery(base, ", \"wall_t\": 0.5", "");
    text = surgery(text, "\"width\": 2, \"length\": [3, 4]", "\"width\": 3, \"length\": [3, 4]");
    delve::Project q;
    std::string err;
    ASSERT_TRUE(loadText(text, q, err)) << err;
    q.dir = std::filesystem::path(DELVE_ASSETS_DIR).parent_path().string();

    delve::layout::Catalog cat;
    ASSERT_TRUE(delve::layout::build_catalog(q, cat, err)) << err;
    delve::layout::LayoutGenerator gen;
    delve::layout::LayoutResult lr;
    delve::layout::GenerateOptions opts;
    opts.attempts = 4;
    ASSERT_TRUE(gen.generate(q, cat, opts, lr, err)) << err;
    std::string ltext;
    ASSERT_TRUE(delve::layout::write_layout_json(lr, q, "chain", ltext, err)) << err;
    delve::LayoutData ld;
    ASSERT_TRUE(delve::read_layout_json(ltext, ld, err)) << err;
    // The corridor room adopted a 3-wide parametric template.
    bool saw_wide = false;
    for (const auto& r : ld.rooms)
        if (r.id == "c1") {
            EXPECT_EQ(r.tmpl.rfind("corridor_3x", 0), 0u) << r.tmpl;
            saw_wide = true;
        }
    ASSERT_TRUE(saw_wide) << "corridor room c1 missing from the layout";

    delve::IrV2 ir3;
    ASSERT_TRUE(delve::build_ir_from_layout(ld, q, "chain", ir3, err)) << err;
    const delve::FillResult fill3 = fillIr(ir3, q);
    std::vector<delve::CheckDiag> ds3;
    EXPECT_TRUE(delve::check_level(ir3, q, fill3, ds3)) << diagText(ds3);
}

// D3 acceptance (§11), refill mode: editing one room's fill parameter and
// refilling at a frozen layout recomputes only the units whose input changed;
// the other rooms' units come from the cache untouched.
TEST(DelveCheck, RefillAfterRoomParamEditKeepsOthers) {
    delve::Project p = loadD2ChainProject();
    const delve::LayoutData ld = generateLayoutData(p);
    const delve::IrV2 ir1 = buildChainIr(ld, p);

    delve::UnitCache cache;
    fillChain(ir1, p, cache);  // warm

    // hall fill.h 3.5 -> 4.0 is a fill-tier edit: the layout stands, the IR
    // is rebuilt from the same LayoutData with the edited project.
    const std::string base = readFile(std::string(DELVE_TEST_DATA) + "/d2_project.json");
    std::string text = surgery(base, ", \"wall_t\": 0.5", "");
    text = surgery(text, "\"fill\": {\"h\": 3.5}", "\"fill\": {\"h\": 4.0}");
    delve::Project q;
    std::string err;
    ASSERT_TRUE(loadText(text, q, err)) << err;
    q.dir = std::filesystem::path(DELVE_ASSETS_DIR).parent_path().string();
    const delve::IrV2 ir2 = buildChainIr(ld, q);
    const delve::FillResult refill = fillChain(ir2, q, cache);

    // Predicted recomputed set: the hall room unit; walls with a hall side;
    // hall facings; nodes at a vertex incident to a hall wall (h_pillar and
    // hall-looking faces bind heights). Doors bind door_h (unchanged); lamps
    // bind no height (the mount height is a transform, reapplied at assembly
    // even on a cache hit).
    std::set<std::string> want{"room:hall"};
    for (const auto& w : ir2.walls)
        if (w.room_left == "hall" || w.room_right == "hall") want.insert(w.id);
    for (const auto& f : ir2.facings)
        if (f.room == "hall") want.insert(f.id);
    for (const auto& n : ir2.nodes)
        for (const auto& w : ir2.walls)
            if ((w.g0 == n.at || w.g1 == n.at) &&
                (w.room_left == "hall" || w.room_right == "hall"))
                want.insert(n.id);
    EXPECT_EQ(std::set<std::string>(refill.stats.reran.begin(), refill.stats.reran.end()),
              want);
    EXPECT_EQ(refill.stats.reran.size() + refill.stats.reused.size(), refill.units.size());
    // Cache hits are invisible: the refill equals a cold run byte-for-byte...
    delve::UnitCache cold;
    const delve::FillResult fresh = fillChain(ir2, q, cold);
    EXPECT_EQ(*refill.mesh->positions, *fresh.mesh->positions);
    EXPECT_EQ(*refill.anchors->positions, *fresh.anchors->positions);
    // ...and the edited level still passes the geometric checks.
    std::vector<delve::CheckDiag> ds;
    EXPECT_TRUE(delve::check_level(ir2, q, refill, ds)) << diagText(ds);
}

// D3 acceptance (§11), re-layout mode: a re-layout that moves rooms keeps the
// units of rooms with unchanged template/transform/parameters in the cache
// (v3 position-independent ids). A full translation of the level is the
// extreme case: every room moved, yet every unit must hit.
TEST(DelveCheck, RelayoutCacheSurvivesMoves) {
    delve::Project p = loadD2ChainProject();
    const delve::LayoutData ld = generateLayoutData(p);
    const delve::IrV2 ir1 = buildChainIr(ld, p);
    delve::UnitCache cache;
    fillChain(ir1, p, cache);  // warm

    // "Re-layout": the same layout translated by +10 cells on x.
    delve::LayoutData moved = ld;
    for (auto& r : moved.rooms) {
        for (auto& g : r.grid) g.first += 10;
        for (auto& d : r.doors) {
            d.g0.first += 10;
            d.g1.first += 10;
        }
    }
    const delve::IrV2 ir2 = buildChainIr(moved, p);
    // Sanity: the IR really moved, and wall/node ids are position-independent.
    EXPECT_NE(ir1.rooms.front().grid.front(), ir2.rooms.front().grid.front());
    std::set<std::string> wallIds1, wallIds2;
    for (const auto& w : ir1.walls) wallIds1.insert(w.id);
    for (const auto& w : ir2.walls) wallIds2.insert(w.id);
    EXPECT_EQ(wallIds1, wallIds2);

    const delve::FillResult refill = fillChain(ir2, p, cache);
    EXPECT_TRUE(refill.stats.reran.empty())
        << "moved level must hit everywhere, reran: "
        << [&] {
               std::string t;
               for (const auto& id : refill.stats.reran) t += id + " ";
               return t;
           }();
    EXPECT_EQ(refill.stats.reused.size(), refill.units.size());
    // Hits are invisible: the refill equals a cold run byte-for-byte.
    delve::UnitCache cold;
    const delve::FillResult fresh = fillChain(ir2, p, cold);
    ASSERT_EQ(refill.mesh->pointCount(), fresh.mesh->pointCount());
    EXPECT_EQ(*refill.mesh->positions, *fresh.mesh->positions);
    EXPECT_EQ(*refill.anchors->positions, *fresh.anchors->positions);
    std::vector<delve::CheckDiag> ds;
    EXPECT_TRUE(delve::check_level(ir2, p, refill, ds)) << diagText(ds);
}

}  // namespace

// Regression probe: the gate door variant must not carry coincident faces
// (the frozen IR only ever has open doors, so check_level never saw a gate
// before D2.3b wired passage dtypes through).
TEST(DelveCheck, GateAssetHasNoDoubleGeometry) {
    const std::string assets = DELVE_ASSETS_DIR;
    const std::string dir = assets + "/doors";
    std::string err;
    pgg::RunParams rp;
    rp.importRoots = {assets};
    for (const auto& [name, text] :
         std::vector<std::pair<std::string, std::string>>{
             {"seg", "@opening_v1.seg.points.json"}, {"h", "2.2"}, {"frame", "0.15"},
             {"thick", "0.6"}, {"dtype", "2"}, {"rng_seed", "7"}}) {
        pgg::Value v;
        ASSERT_TRUE(pgg::parseParamText(text, dir, v, &err)) << err;
        rp.values.emplace_back(name, v);
    }
    const pgg::RunResult r = pgg::runFile(dir + "/opening_v1.pgg", rp);
    ASSERT_FALSE(r.hasErrors());
    delve::FillResult fill;
    for (const auto& o : r.outputs)
        if (o.name == "mesh") fill.mesh = pgg::asGeo(o.value);
    ASSERT_NE(fill.mesh, nullptr);
    // The door asset is style-neutral by contract; the F6 merge gives its
    // points a neutral 0 column (slots §1). Mimic that here.
    auto styled = std::make_shared<pgg::Geo>(*fill.mesh);
    auto attrs = std::make_shared<pgg::AttrSet>(
        fill.mesh->pointAttrs ? *fill.mesh->pointAttrs : pgg::AttrSet{});
    attrs->columns["style"] = pgg::AttrColumn{
        std::make_shared<const std::vector<int64_t>>(fill.mesh->pointCount(), 0)};
    styled->pointAttrs = std::move(attrs);
    fill.mesh = std::move(styled);
    delve::FillResult::UnitSpan span;
    span.id = "door:t";
    span.slot = "door";
    span.meshBegin = 0;
    span.meshEnd = fill.mesh->pointCount();
    fill.units.push_back(span);
    std::vector<delve::CheckDiag> ds;
    EXPECT_TRUE(delve::check_elements(fill, ds)) << diagText(ds);
}

// A1: the unit-scoped precheck (check --unit / RPC unit) runs the per-unit
// elements logic only on units whose id contains the filter substring; zero
// matches is an error (probably a typo in the filter).
TEST(DelveCheck, CheckUnitsFiltersSpans) {
    // Three quads: "unit:a" is clean; "unit:b" carries two coincident
    // same-normal quads (double geometry).
    const std::vector<glm::vec3> pos = {
        {0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1},  // quad A
        {2, 0, 0}, {3, 0, 0}, {3, 0, 1}, {2, 0, 1},  // quad B1
        {2, 0, 0}, {3, 0, 0}, {3, 0, 1}, {2, 0, 1},  // quad B2 = B1
    };
    const std::vector<int32_t> corners = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    const std::vector<int32_t> offs = {0, 4, 8, 12};
    delve::FillResult fill;
    fill.mesh = pgg::makeMesh(pos, corners, offs);
    auto styled = std::make_shared<pgg::Geo>(*fill.mesh);
    auto attrs = std::make_shared<pgg::AttrSet>();
    attrs->columns["style"] =
        pgg::AttrColumn{std::make_shared<const std::vector<int64_t>>(12, 1)};
    styled->pointAttrs = std::move(attrs);
    fill.mesh = std::move(styled);
    delve::FillResult::UnitSpan a;
    a.id = "unit:a";
    a.slot = "facing";
    a.meshBegin = 0;
    a.meshEnd = 4;
    delve::FillResult::UnitSpan b;
    b.id = "unit:b";
    b.slot = "facing";
    b.meshBegin = 4;
    b.meshEnd = 12;
    fill.units = {a, b};

    // Full-level elements check sees the dirty unit.
    {
        std::vector<delve::CheckDiag> ds;
        EXPECT_FALSE(delve::check_elements(fill, ds));
        EXPECT_TRUE(hasDiag(ds, "elements", "coincident faces")) << diagText(ds);
    }
    // The clean unit alone passes and reports exactly one match.
    {
        std::vector<delve::CheckDiag> ds;
        size_t matched = 0;
        EXPECT_TRUE(delve::check_units(fill, "unit:a", ds, &matched)) << diagText(ds);
        EXPECT_EQ(matched, 1u);
        EXPECT_TRUE(ds.empty()) << diagText(ds);
    }
    // The dirty unit alone is caught.
    {
        std::vector<delve::CheckDiag> ds;
        size_t matched = 0;
        EXPECT_FALSE(delve::check_units(fill, "unit:b", ds, &matched));
        EXPECT_EQ(matched, 1u);
        EXPECT_TRUE(hasDiag(ds, "elements", "coincident faces")) << diagText(ds);
    }
    // A substring matching both runs both.
    {
        std::vector<delve::CheckDiag> ds;
        size_t matched = 0;
        EXPECT_FALSE(delve::check_units(fill, "unit:", ds, &matched));
        EXPECT_EQ(matched, 2u);
    }
    // Zero matches: error with a hint, not a silent pass.
    {
        std::vector<delve::CheckDiag> ds;
        size_t matched = 99;
        EXPECT_FALSE(delve::check_units(fill, "no_such_unit", ds, &matched));
        EXPECT_EQ(matched, 0u);
        EXPECT_TRUE(hasDiag(ds, "elements", "matches no unit")) << diagText(ds);
    }
}

// B3: elements verdicts are cached per F8 unit key — replay is governed by
// the key (and the rule-set version), not by the geometry currently in fill.
TEST(DelveCheck, ElementsCachedVerdicts) {
    const std::vector<glm::vec3> pos = {
        {0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1},  // quad A (clean)
        {2, 0, 0}, {3, 0, 0}, {3, 0, 1}, {2, 0, 1},  // quad B1
        {2, 0, 0}, {3, 0, 0}, {3, 0, 1}, {2, 0, 1},  // quad B2 = B1 (dirty)
    };
    const std::vector<int32_t> corners = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    const std::vector<int32_t> offs = {0, 4, 8, 12};
    auto makeFill = [&](std::vector<delve::FillResult::UnitSpan> spans) {
        delve::FillResult fill;
        fill.mesh = pgg::makeMesh(pos, corners, offs);
        auto styled = std::make_shared<pgg::Geo>(*fill.mesh);
        auto attrs = std::make_shared<pgg::AttrSet>();
        attrs->columns["style"] =
            pgg::AttrColumn{std::make_shared<const std::vector<int64_t>>(12, 1)};
        styled->pointAttrs = std::move(attrs);
        fill.mesh = std::move(styled);
        fill.units = std::move(spans);
        return fill;
    };
    auto mkSpan = [](const std::string& id, size_t begin, size_t end, uint64_t key) {
        delve::FillResult::UnitSpan s;
        s.id = id;
        s.slot = "facing";
        s.meshBegin = begin;
        s.meshEnd = end;
        s.cacheKey = key;
        return s;
    };
    // A clean variant: the duplicate quad removed (B occupies [4,8)).
    auto makeCleanFill = [&](std::vector<delve::FillResult::UnitSpan> spans) {
        delve::FillResult fill;
        fill.mesh = pgg::makeMesh({pos.begin(), pos.begin() + 8}, {0, 1, 2, 3, 4, 5, 6, 7},
                                  {0, 4, 8});
        auto styled = std::make_shared<pgg::Geo>(*fill.mesh);
        auto attrs = std::make_shared<pgg::AttrSet>();
        attrs->columns["style"] =
            pgg::AttrColumn{std::make_shared<const std::vector<int64_t>>(8, 1)};
        styled->pointAttrs = std::move(attrs);
        fill.mesh = std::move(styled);
        fill.units = std::move(spans);
        return fill;
    };

    delve::UnitCache cache;
    cache.store(delve::UnitKey{101}, pgg::makePoints({}), pgg::makePoints({}));
    cache.store(delve::UnitKey{102}, pgg::makePoints({}), pgg::makePoints({}));

    // First run computes live: the dirty span is caught and both verdicts land
    // in the cache.
    {
        delve::FillResult fill =
            makeFill({mkSpan("unit:a", 0, 4, 101), mkSpan("unit:b", 4, 12, 102)});
        std::vector<delve::CheckDiag> ds;
        EXPECT_FALSE(delve::check_elements_cached(fill, &cache, ds));
        EXPECT_TRUE(hasDiag(ds, "elements", "unit:b: coincident faces")) << diagText(ds);
        EXPECT_FALSE(hasDiag(ds, "elements", "unit:a")) << diagText(ds);
    }
    // Replay is key-governed: the same keys over a CLEAN mesh still report the
    // cached dirty verdict (the caller guarantees key <=> output identity).
    {
        delve::FillResult fill =
            makeCleanFill({mkSpan("unit:a", 0, 4, 101), mkSpan("unit:b", 4, 8, 102)});
        std::vector<delve::CheckDiag> ds;
        EXPECT_FALSE(delve::check_elements_cached(fill, &cache, ds)) << diagText(ds);
        EXPECT_TRUE(hasDiag(ds, "elements", "unit:b: coincident faces")) << diagText(ds);
    }
    // Replay re-prefixes the current span id (same key under another id).
    {
        delve::FillResult fill = makeCleanFill({mkSpan("unit:c", 4, 8, 102)});
        std::vector<delve::CheckDiag> ds;
        EXPECT_FALSE(delve::check_elements_cached(fill, &cache, ds));
        EXPECT_TRUE(hasDiag(ds, "elements", "unit:c: coincident faces")) << diagText(ds);
        EXPECT_FALSE(hasDiag(ds, "elements", "unit:b")) << diagText(ds);
    }
    // A stale rule-set version is a miss: the verdict is recomputed live
    // (clean mesh now -> ok).
    {
        delve::UnitCache stale;
        stale.store(delve::UnitKey{101}, pgg::makePoints({}), pgg::makePoints({}));
        stale.storeCheck(delve::UnitKey{101},
                         delve::UnitCache::CheckVerdict{delve::kElementsCheckVersion + 1,
                                                        {"stale finding"}});
        delve::FillResult fill = makeCleanFill({mkSpan("unit:a", 0, 4, 101)});
        std::vector<delve::CheckDiag> ds;
        EXPECT_TRUE(delve::check_elements_cached(fill, &stale, ds)) << diagText(ds);
        EXPECT_FALSE(hasDiag(ds, "elements", "stale finding")) << diagText(ds);
    }
    // Key-less spans and a null cache behave exactly like check_elements.
    {
        delve::FillResult fill = makeFill({mkSpan("unit:a", 0, 4, 0), mkSpan("unit:b", 4, 12, 0)});
        std::vector<delve::CheckDiag> noCache, keyless, plain;
        EXPECT_FALSE(delve::check_elements_cached(fill, nullptr, noCache));
        EXPECT_FALSE(delve::check_elements_cached(fill, &cache, keyless));
        EXPECT_FALSE(delve::check_elements(fill, plain));
        EXPECT_EQ(noCache.size(), plain.size());
        EXPECT_EQ(keyless.size(), plain.size());
    }
}
