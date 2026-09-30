// Delve D1.4-D1.5: F11 geometric checks over the filled level + transition
// paint (butt/chase x corner/wall on the stone|brick corner scene).

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

#include "pgg/src/eval/geometry.h"

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

size_t checkPaint(const delve::IrV1& ir, const delve::FillResult& fill,
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
size_t checkNodePaint(const delve::IrV1& ir, const delve::FillResult& fill,
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
            const delve::IrV1 ir =
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

}  // namespace
