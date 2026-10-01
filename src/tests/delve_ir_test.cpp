// Delve D1.1: project side-rules + IR v2 (F4) + delve-ir/2 JSON round-trip.

#include <gtest/gtest.h>

#include <cstdio>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

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

// Replace the unique `from` substring; fails the test when absent/ambiguous.
std::string surgery(const std::string& text, const std::string& from, const std::string& to) {
    const size_t first = text.find(from);
    EXPECT_NE(first, std::string::npos) << "anchor missing: " << from;
    EXPECT_EQ(text.find(from, first + 1), std::string::npos) << "anchor ambiguous: " << from;
    std::string out = text;
    out.replace(first, from.size(), to);
    return out;
}

void writeFile(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

delve::Project loadFixtureProject() {
    delve::Project p;
    std::string err;
    const std::string path = std::string(DELVE_TEST_DATA) + "/d1_project.json";
    EXPECT_TRUE(delve::load_project(path, p, err)) << err;
    return p;
}

const delve::IrFacing* findFacing(const delve::IrV2& ir, const std::string& id) {
    for (const auto& f : ir.facings)
        if (f.id == id) return &f;
    return nullptr;
}

const delve::IrNode* findNode(const delve::IrV2& ir, const std::string& id) {
    for (const auto& n : ir.nodes)
        if (n.id == id) return &n;
    return nullptr;
}

const delve::IrNode* findNodeAt(const delve::IrV2& ir, delve::GridPt at) {
    for (const auto& n : ir.nodes)
        if (n.at == at) return &n;
    return nullptr;
}

}  // namespace

TEST(ProjectRules, SideResolution) {
    const delve::Project p = loadFixtureProject();
    ASSERT_EQ(p.fill.side_rules.size(), 2u);
    // (room_role, outer, adjacent_role) -> style
    EXPECT_EQ(delve::resolve_side_style(p, "hall", true, ""), "stone");
    EXPECT_EQ(delve::resolve_side_style(p, "hall", false, "corridor"), "brick");
    EXPECT_EQ(delve::resolve_side_style(p, "hall", false, "hall"), "stone");
    // Outer sides never match adjacent_role.
    EXPECT_EQ(delve::resolve_side_style(p, "corridor", true, ""), "stone");
    EXPECT_EQ(delve::resolve_side_style(p, "corridor", false, "hall"), "brick");
}

TEST(ProjectRules, LastWins) {
    delve::Project p = loadFixtureProject();
    delve::SideRule r;
    r.side = "outer";
    r.style = "brick";
    p.fill.side_rules.push_back(r);
    EXPECT_EQ(delve::resolve_side_style(p, "hall", true, ""), "brick");
    EXPECT_EQ(delve::resolve_side_style(p, "hall", false, "hall"), "stone");
}

TEST(ProjectRules, BadRuleRejected) {
    const std::string dir = testing::TempDir();
    const std::string base = readFile(std::string(DELVE_TEST_DATA) + "/d1_project.json");
    ASSERT_FALSE(base.empty());
    // Unknown style.
    {
        std::string bad = base;
        const std::string from = "{\"match\": {\"side\": \"outer\"}, \"style\": \"stone\"}";
        ASSERT_NE(bad.find(from), std::string::npos);
        bad.replace(bad.find(from), from.size(),
                    "{\"match\": {\"side\": \"outer\"}, \"style\": \"gold\"}");
        const std::string path = dir + "/bad_rule_style.json";
        writeFile(path, bad);
        delve::Project p;
        std::string err;
        EXPECT_FALSE(delve::load_project(path, p, err));
        EXPECT_NE(err.find("side_rules"), std::string::npos) << err;
    }
    // Unknown match key.
    {
        std::string bad = base;
        const std::string from = "\"adjacent_role\": \"corridor\"";
        ASSERT_NE(bad.find(from), std::string::npos);
        bad.replace(bad.find(from), from.size(), "\"adjacent\": \"corridor\"");
        const std::string path = dir + "/bad_rule_key.json";
        writeFile(path, bad);
        delve::Project p;
        std::string err;
        EXPECT_FALSE(delve::load_project(path, p, err));
        EXPECT_NE(err.find("unknown key"), std::string::npos) << err;
    }
}

TEST(IrRealLayout, WallsNodesDoors) {
    const delve::Project p = loadFixtureProject();
    const std::string frozen_path = std::string(DELVE_D0_DIR) + "/frozen_ir.json";
    delve::IrV2 ir;
    std::string err;
    ASSERT_TRUE(delve::build_ir_v2(readFile(frozen_path), frozen_path, p, "test", ir, err)) << err;

    EXPECT_EQ(ir.rooms.size(), 17u);
    // Atom counts verified against an independent sweep of the frozen IR.
    EXPECT_EQ(ir.walls.size(), 87u);
    size_t shared = 0, outer = 0;
    for (const auto& w : ir.walls) (w.outer ? outer : shared)++;
    EXPECT_EQ(shared, 21u);
    EXPECT_EQ(outer, 66u);
    EXPECT_EQ(ir.doors.size(), 20u);

    // F4: every shared wall = one body + one owner + two facings; outer = one facing.
    for (const auto& w : ir.walls) {
        size_t facings = 0;
        for (const auto& f : ir.facings)
            if (f.wall == w.id) facings++;
        EXPECT_EQ(facings, w.outer ? 1u : 2u) << w.id;
        EXPECT_FALSE(w.owner.empty()) << w.id;
        if (!w.outer) {
            EXPECT_EQ(w.owner, std::min(w.room_left, w.room_right)) << w.id;
            EXPECT_NE(w.room_left, w.room_right) << w.id;
        }
    }
    // F4: every vertex where walls meet = exactly one node; v3 ids are
    // position-independent: node:<owner>:<v>, wall:<owner>:<edge>[.<k>] (D3).
    std::set<std::pair<int, int>> ends;
    for (const auto& w : ir.walls) {
        ends.insert(w.g0);
        ends.insert(w.g1);
        EXPECT_EQ(w.id.rfind("wall:" + w.owner + ":", 0), 0u) << w.id;
    }
    EXPECT_EQ(ir.nodes.size(), ends.size());
    std::set<std::string> nodeIds;
    for (const auto& e : ends) {
        const delve::IrNode* n = findNodeAt(ir, e);
        ASSERT_NE(n, nullptr) << e.first << "," << e.second;
        EXPECT_EQ(n->id.rfind("node:" + n->owner + ":", 0), 0u) << n->id;
        nodeIds.insert(n->id);
    }
    EXPECT_EQ(nodeIds.size(), ir.nodes.size()) << "node ids must be unique";
    // No corridors in this layout, no style changes -> no transitions, no warnings.
    EXPECT_TRUE(ir.transitions.empty());
    EXPECT_TRUE(ir.warnings.empty());
    EXPECT_TRUE(ir.corridor_clear.empty());

    // Door cuts agree with the door record (same full segment, meters).
    for (const auto& d : ir.doors) {
        EXPECT_DOUBLE_EQ(d.clear, 2.0 - 2.0 * 0.15);
        EXPECT_EQ(d.dtype, 1);
        for (const auto& f : ir.facings) {
            if (f.wall != d.wall) continue;
            ASSERT_EQ(f.cuts.size(), 1u) << f.id;
            // Full segment: door grid span in meters on the face plane.
            const double cut_len =
                std::hypot(f.cuts[0].b.first - f.cuts[0].a.first, f.cuts[0].b.second - f.cuts[0].a.second);
            EXPECT_DOUBLE_EQ(cut_len, 2.0) << f.id;
            EXPECT_DOUBLE_EQ(f.cuts[0].h, 2.2) << f.id;
        }
    }
}

TEST(IrRealLayout, ForcedTransitions) {
    delve::Project p = loadFixtureProject();
    delve::SideRule r;
    r.side = "outer";
    r.style = "brick";  // outer brick vs shared stone -> transitions at every joint
    p.fill.side_rules.push_back(r);
    const std::string frozen_path = std::string(DELVE_D0_DIR) + "/frozen_ir.json";
    delve::IrV2 ir;
    std::string err;
    ASSERT_TRUE(delve::build_ir_v2(readFile(frozen_path), frozen_path, p, "test", ir, err)) << err;
    EXPECT_GT(ir.transitions.size(), 0u);

    // Every transition is referenced by >= 1 piece; every piece references a live zone.
    std::set<int> live;
    for (const auto& t : ir.transitions) live.insert(t.id);
    std::set<int> used;
    auto check_pieces = [&](const std::vector<delve::ZonePiece>& pieces, bool is_face) {
        for (const auto& z : pieces) {
            EXPECT_TRUE(live.count(z.zone)) << z.zone;
            used.insert(z.zone);
            EXPECT_EQ(z.pattern, 0);
            if (!is_face)
                EXPECT_EQ(z.flip, 0);  // facings run with +s; faces carry parity
            else
                EXPECT_TRUE(z.flip == 0 || z.flip == 1);
            EXPECT_DOUBLE_EQ(z.width, 1.0);
            EXPECT_DOUBLE_EQ(z.module, 0.25);
            EXPECT_LT(z.l0, z.l1);
            EXPECT_EQ(z.seed, delve::zone_seed(z.zone));
        }
    };
    for (const auto& f : ir.facings) check_pieces(f.zones, false);
    for (const auto& n : ir.nodes)
        for (const auto& f : n.faces) check_pieces(f.zones, true);
    EXPECT_EQ(used, live);
    // Transition ids are dense and ordered by (room, s0).
    for (size_t i = 0; i < ir.transitions.size(); ++i) EXPECT_EQ(ir.transitions[i].id, (int)i);
}

TEST(IrCorner, ButtCentered) {
    const delve::Project p = loadFixtureProject();  // butt + corner
    const std::string frozen_path = std::string(DELVE_TEST_DATA) + "/corner_frozen.json";
    delve::IrV2 ir;
    std::string err;
    ASSERT_TRUE(delve::build_ir_v2(readFile(frozen_path), frozen_path, p, "test", ir, err)) << err;

    ASSERT_EQ(ir.rooms.size(), 2u);
    EXPECT_EQ(ir.walls.size(), 9u);
    EXPECT_EQ(ir.facings.size(), 10u);
    EXPECT_EQ(ir.nodes.size(), 8u);
    ASSERT_EQ(ir.doors.size(), 1u);
    EXPECT_EQ(ir.doors[0].id, "door:0-1");
    EXPECT_DOUBLE_EQ(ir.doors[0].clear, 1.7);
    // Transitions 2,3 (room 1 corners) lose their pillar gaps -> shortened.
    ASSERT_EQ(ir.warnings.size(), 2u);
    ASSERT_EQ(ir.corridor_clear.size(), 1u);
    EXPECT_DOUBLE_EQ(ir.corridor_clear.at("1"), 3.4);

    // Room 0 bottom edge (walk -x, s in [20, 32]): stone | brick | stone.
    const delve::IrFacing* f0 = findFacing(ir, "fac:0:2.0");
    const delve::IrFacing* f1 = findFacing(ir, "fac:0:2.1");
    const delve::IrFacing* f2 = findFacing(ir, "fac:0:2.2");
    ASSERT_NE(f0, nullptr);
    ASSERT_NE(f1, nullptr);
    ASSERT_NE(f2, nullptr);
    EXPECT_EQ(f0->style, "stone");
    EXPECT_EQ(f1->style, "brick");
    EXPECT_EQ(f2->style, "stone");
    EXPECT_DOUBLE_EQ(f0->s0, 20.3);
    EXPECT_DOUBLE_EQ(f0->s1, 21.7);
    EXPECT_DOUBLE_EQ(f1->s0, 22.3);
    EXPECT_DOUBLE_EQ(f1->s1, 29.7);
    ASSERT_EQ(f1->cuts.size(), 1u);
    EXPECT_DOUBLE_EQ(f1->cuts[0].h, 2.2);

    // Four transitions: two T-joints in room 0, two corners in room 1
    // (corridor outers are stone by rule, the shared wall is brick by role).
    ASSERT_EQ(ir.transitions.size(), 4u);
    EXPECT_EQ(ir.transitions[0].room, "0");
    EXPECT_EQ(ir.transitions[0].style_a, "stone");
    EXPECT_EQ(ir.transitions[0].style_b, "brick");
    EXPECT_DOUBLE_EQ(ir.transitions[0].s0, 21.5);
    EXPECT_DOUBLE_EQ(ir.transitions[0].s1, 22.5);
    EXPECT_FALSE(ir.transitions[0].shortened);
    EXPECT_EQ(ir.transitions[1].style_a, "brick");
    EXPECT_EQ(ir.transitions[1].style_b, "stone");
    EXPECT_DOUBLE_EQ(ir.transitions[1].s0, 29.5);
    EXPECT_DOUBLE_EQ(ir.transitions[1].s1, 30.5);
    EXPECT_FALSE(ir.transitions[1].shortened);
    // Room 1 corner @ s=8 (plain corner: the pillar gap shortens the zone).
    EXPECT_EQ(ir.transitions[2].room, "1");
    EXPECT_EQ(ir.transitions[2].style_a, "brick");
    EXPECT_EQ(ir.transitions[2].style_b, "stone");
    EXPECT_DOUBLE_EQ(ir.transitions[2].s0, 7.5);
    EXPECT_DOUBLE_EQ(ir.transitions[2].s1, 8.5);
    EXPECT_TRUE(ir.transitions[2].shortened);
    // Room 1 wrap corner @ s=24 (zone crosses the development origin).
    EXPECT_EQ(ir.transitions[3].room, "1");
    EXPECT_EQ(ir.transitions[3].style_a, "stone");
    EXPECT_EQ(ir.transitions[3].style_b, "brick");
    EXPECT_DOUBLE_EQ(ir.transitions[3].s0, 23.5);
    EXPECT_DOUBLE_EQ(ir.transitions[3].s1, 24.5);
    EXPECT_TRUE(ir.transitions[3].shortened);

    // Pieces of transition 0: facing runs + the T-face run.
    ASSERT_EQ(f0->zones.size(), 1u);
    EXPECT_EQ(f0->zones[0].zone, 0);
    EXPECT_EQ(f0->zones[0].style_a, 1);  // stone
    EXPECT_EQ(f0->zones[0].style_b, 2);  // brick
    EXPECT_NEAR(f0->zones[0].l0, 1.2, 1e-9);
    EXPECT_NEAR(f0->zones[0].l1, 1.4, 1e-9);
    EXPECT_NEAR(f0->zones[0].t_at_l0, -1.2, 1e-9);
    const delve::IrNode* n5 = findNodeAt(ir, {5, 0});
    ASSERT_NE(n5, nullptr);
    const delve::IrNodeFace* tf = nullptr;
    for (const auto& f : n5->faces)
        if (f.room == "0") tf = &f;
    ASSERT_NE(tf, nullptr);
    EXPECT_EQ(tf->style, "stone");  // A side
    ASSERT_EQ(tf->zones.size(), 1u);
    EXPECT_NEAR(tf->zones[0].l0, -0.3, 1e-9);
    EXPECT_NEAR(tf->zones[0].l1, 0.3, 1e-9);
    EXPECT_NEAR(tf->zones[0].t_at_l0, 0.5, 1e-9);
    EXPECT_EQ(tf->zones[0].flip, 1);  // face +x opposes +s here
    // Transition 1 pieces on f1 (second piece: f1 also closes transition 0) and f2.
    ASSERT_EQ(f1->zones.size(), 2u);
    EXPECT_EQ(f1->zones[0].zone, 0);
    EXPECT_NEAR(f1->zones[0].l0, 0.0, 1e-9);
    EXPECT_NEAR(f1->zones[0].l1, 0.2, 1e-9);
    EXPECT_NEAR(f1->zones[0].t_at_l0, 0.8, 1e-9);
    EXPECT_EQ(f1->zones[1].zone, 1);
    EXPECT_EQ(f1->zones[1].style_a, 2);  // brick
    EXPECT_EQ(f1->zones[1].style_b, 1);  // stone
    EXPECT_NEAR(f1->zones[1].l0, 7.2, 1e-9);
    EXPECT_NEAR(f1->zones[1].l1, 7.4, 1e-9);
    EXPECT_NEAR(f1->zones[1].t_at_l0, -7.2, 1e-9);
    ASSERT_EQ(f2->zones.size(), 1u);
    EXPECT_EQ(f2->zones[0].zone, 1);

    // Room 1 corner pieces: fac:1:0 hosts runs of transitions 2 and 3.
    const delve::IrFacing* g0 = findFacing(ir, "fac:1:0");
    const delve::IrFacing* g1 = findFacing(ir, "fac:1:1");
    const delve::IrFacing* g3 = findFacing(ir, "fac:1:3");
    ASSERT_NE(g0, nullptr);
    ASSERT_NE(g1, nullptr);
    ASSERT_NE(g3, nullptr);
    ASSERT_EQ(g0->zones.size(), 2u);
    EXPECT_EQ(g0->zones[0].zone, 3);  // ascending l: wrap run first
    EXPECT_NEAR(g0->zones[0].l0, 0.0, 1e-9);
    EXPECT_NEAR(g0->zones[0].l1, 0.2, 1e-9);
    EXPECT_NEAR(g0->zones[0].t_at_l0, 0.8, 1e-9);  // continuous t across the wrap
    EXPECT_EQ(g0->zones[1].zone, 2);
    EXPECT_NEAR(g0->zones[1].l0, 7.2, 1e-9);
    EXPECT_NEAR(g0->zones[1].l1, 7.4, 1e-9);
    ASSERT_EQ(g1->zones.size(), 1u);
    EXPECT_EQ(g1->zones[0].zone, 2);
    EXPECT_NEAR(g1->zones[0].l0, 0.0, 1e-9);
    EXPECT_NEAR(g1->zones[0].l1, 0.2, 1e-9);
    ASSERT_EQ(g3->zones.size(), 1u);
    EXPECT_EQ(g3->zones[0].zone, 3);
    EXPECT_NEAR(g3->zones[0].l0, 3.2, 1e-9);
    EXPECT_NEAR(g3->zones[0].l1, 3.4, 1e-9);
    EXPECT_NEAR(g3->zones[0].t_at_l0, -3.2, 1e-9);
}

TEST(IrCorner, ChaseOnWall) {
    delve::Project p = loadFixtureProject();
    p.fill.transitions.pattern = "chase";
    p.fill.transitions.place = "wall";
    const std::string frozen_path = std::string(DELVE_TEST_DATA) + "/corner_frozen.json";
    delve::IrV2 ir;
    std::string err;
    ASSERT_TRUE(delve::build_ir_v2(readFile(frozen_path), frozen_path, p, "test", ir, err)) << err;

    ASSERT_EQ(ir.transitions.size(), 4u);
    EXPECT_EQ(ir.transitions[0].pattern, 1);
    EXPECT_EQ(ir.transitions[0].place, "wall");
    // On the B side, starting at the joint edge.
    EXPECT_DOUBLE_EQ(ir.transitions[0].s0, 22.3);
    EXPECT_DOUBLE_EQ(ir.transitions[0].s1, 23.3);
    EXPECT_DOUBLE_EQ(ir.transitions[1].s0, 30.3);
    EXPECT_DOUBLE_EQ(ir.transitions[1].s1, 31.3);
    EXPECT_DOUBLE_EQ(ir.transitions[2].s0, 8.3);
    EXPECT_DOUBLE_EQ(ir.transitions[2].s1, 9.3);
    EXPECT_DOUBLE_EQ(ir.transitions[3].s0, 0.3);
    EXPECT_DOUBLE_EQ(ir.transitions[3].s1, 1.3);
    EXPECT_TRUE(ir.warnings.empty());

    const delve::IrFacing* f1 = findFacing(ir, "fac:0:2.1");
    ASSERT_NE(f1, nullptr);
    ASSERT_EQ(f1->zones.size(), 1u);
    EXPECT_EQ(f1->zones[0].pattern, 1);
    EXPECT_DOUBLE_EQ(f1->zones[0].l0, 0.0);
    EXPECT_DOUBLE_EQ(f1->zones[0].l1, 1.0);
    EXPECT_DOUBLE_EQ(f1->zones[0].t_at_l0, 0.0);
    // Wall placement never touches the T-face.
    const delve::IrNode* n5 = findNodeAt(ir, {5, 0});
    ASSERT_NE(n5, nullptr);
    for (const auto& f : n5->faces) EXPECT_TRUE(f.zones.empty());
}

TEST(IrCorner, RetuneWithoutEdgar) {
    delve::Project p = loadFixtureProject();
    p.fill.cell = 2.5;
    p.fill.wall_t = 0.8;
    const std::string frozen_path = std::string(DELVE_TEST_DATA) + "/corner_frozen.json";
    delve::IrV2 ir;
    std::string err;
    ASSERT_TRUE(delve::build_ir_v2(readFile(frozen_path), frozen_path, p, "test", ir, err)) << err;
    ASSERT_EQ(ir.doors.size(), 1u);
    EXPECT_DOUBLE_EQ(ir.doors[0].clear, 2.2);  // 2.5 - 2*0.15
    const delve::IrFacing* f0 = findFacing(ir, "fac:0:2.0");
    ASSERT_NE(f0, nullptr);
    EXPECT_DOUBLE_EQ(f0->s0, 25.4);  // 20*1.25 + 0.4
    EXPECT_DOUBLE_EQ(f0->s1, 27.1);  // 25 + 1*2.5 - 0.4
    EXPECT_DOUBLE_EQ(f0->from.first, 14.6);  // 6*2.5 - 0.4
    // Side normal points INTO the room (D2.3b fix): room 0 is z >= 0, so its
    // bottom-wall facing sits at +wall_t/2 (was -0.4 on the far side before).
    EXPECT_DOUBLE_EQ(f0->from.second, 0.4);
    EXPECT_DOUBLE_EQ(f0->n.second, 1.0);
}

TEST(IrErrors, DoorOffset) {
    const delve::Project p = loadFixtureProject();
    std::string text = readFile(std::string(DELVE_TEST_DATA) + "/corner_frozen.json");
    const std::string from = "[[2, 0], [3, 0]]";
    ASSERT_NE(text.find(from), std::string::npos);
    // Move both door entries to the atom edge: offset 0 cells.
    for (size_t pos = 0; (pos = text.find(from, pos)) != std::string::npos;) {
        text.replace(pos, from.size(), "[[1, 0], [2, 0]]");
        pos += 1;
    }
    delve::IrV2 ir;
    std::string err;
    EXPECT_FALSE(delve::build_ir_v2(text, "corner", p, "test", ir, err));
    EXPECT_NE(err.find("door:0-1"), std::string::npos) << err;
    EXPECT_NE(err.find("5.4"), std::string::npos) << err;
}

TEST(IrErrors, UnpairedDoor) {
    const delve::Project p = loadFixtureProject();
    std::string text = readFile(std::string(DELVE_TEST_DATA) + "/corner_frozen.json");
    const std::string from = "\"to\": 0, \"grid\": [[2, 0], [3, 0]]";
    ASSERT_NE(text.find(from), std::string::npos);
    text.replace(text.find(from), from.size(), "\"to\": 0, \"grid\": [[4, 0], [5, 0]]");
    delve::IrV2 ir;
    std::string err;
    EXPECT_FALSE(delve::build_ir_v2(text, "corner", p, "test", ir, err));
    EXPECT_NE(err.find("no matching entry"), std::string::npos) << err;
}

TEST(IrErrors, MultiCellDoor) {
    const delve::Project p = loadFixtureProject();
    std::string text = readFile(std::string(DELVE_TEST_DATA) + "/corner_frozen.json");
    const std::string from = "[[2, 0], [3, 0]]";
    for (size_t pos = 0; (pos = text.find(from, pos)) != std::string::npos;) {
        text.replace(pos, from.size(), "[[2, 0], [4, 0]]");
        pos += 1;
    }
    // Fix one room back so pairing still fails on length first (length is checked first).
    delve::IrV2 ir;
    std::string err;
    EXPECT_FALSE(delve::build_ir_v2(text, "corner", p, "test", ir, err));
    EXPECT_NE(err.find("multi-cell"), std::string::npos) << err;
}

TEST(IrErrors, FiguredRoom) {
    const delve::Project p = loadFixtureProject();
    std::string text = readFile(std::string(DELVE_TEST_DATA) + "/corner_frozen.json");
    const std::string from = "[[0, 0], [6, 0], [6, 4], [0, 4]]";
    ASSERT_NE(text.find(from), std::string::npos);
    text.replace(text.find(from), from.size(), "[[0, 0], [6, 0], [6, 4], [3, 4], [0, 4]]");
    delve::IrV2 ir;
    std::string err;
    EXPECT_FALSE(delve::build_ir_v2(text, "corner", p, "test", ir, err));
    EXPECT_NE(err.find("figured rooms"), std::string::npos) << err;
}

TEST(IrJson, RoundTrip) {
    const delve::Project p = loadFixtureProject();
    const std::string frozen_path = std::string(DELVE_TEST_DATA) + "/corner_frozen.json";
    delve::IrV2 ir;
    std::string err;
    ASSERT_TRUE(delve::build_ir_v2(readFile(frozen_path), frozen_path, p, "test", ir, err)) << err;
    std::string t1, t2;
    ASSERT_TRUE(delve::write_ir_v2_json(ir, t1, err)) << err;
    delve::IrV2 back;
    ASSERT_TRUE(delve::read_ir_v2_json(t1, back, err)) << err;
    ASSERT_TRUE(delve::write_ir_v2_json(back, t2, err)) << err;
    EXPECT_EQ(t1, t2);  // byte-stable (N1/N6)
    EXPECT_EQ(back.rooms.size(), 2u);
    EXPECT_EQ(back.walls.size(), 9u);
    EXPECT_EQ(back.transitions.size(), 4u);
    EXPECT_EQ(back.facings.size(), 10u);
    // F12: provenance chains survive the roundtrip.
    ASSERT_FALSE(back.rooms[0].prov.empty());
    EXPECT_EQ(back.rooms[0].prov.at("h").back().level, "role");
    const delve::IrFacing* f = findFacing(back, "fac:0:2.1");
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(f->prov.at("style").size(), 2u);
    EXPECT_EQ(f->prov.at("style").back().level, "side");
    EXPECT_EQ(f->prov.at("style").back().detail, "side_rules[0] (adjacent_role=corridor)");
    EXPECT_EQ(delve::format_prov(f->prov.at("style")),
              "brick <- side \"side_rules[0] (adjacent_role=corridor)\" <- role \"*\" (stone)");
    ASSERT_EQ(back.doors.size(), 1u);
    EXPECT_EQ(back.doors[0].prov.at("dtype").back().level, "default");  // frozen path
    EXPECT_EQ(back.transitions[0].prov.at("pattern").back().level, "project");
}

TEST(IrJson, RejectsOtherFormats) {
    delve::IrV2 ir;
    std::string err;
    EXPECT_FALSE(delve::read_ir_v2_json("{", ir, err));
    EXPECT_NE(err.find("invalid JSON"), std::string::npos) << err;
    const std::string frozen = readFile(std::string(DELVE_D0_DIR) + "/frozen_ir.json");
    ASSERT_FALSE(frozen.empty());
    EXPECT_FALSE(delve::read_ir_v2_json(frozen, ir, err));  // delve-ir/0 is not v2
    EXPECT_NE(err.find("delve-ir/2"), std::string::npos) << err;
    EXPECT_FALSE(delve::read_ir_v2_json(R"({"format": "delve-ir/1"})", ir, err));
    EXPECT_NE(err.find("int room ids"), std::string::npos) << err;  // N7 hint
}

TEST(IrJson, ReadsV2WithoutProv) {
    const delve::Project p = loadFixtureProject();
    const std::string frozen_path = std::string(DELVE_TEST_DATA) + "/corner_frozen.json";
    delve::IrV2 ir;
    std::string err;
    ASSERT_TRUE(delve::build_ir_v2(readFile(frozen_path), frozen_path, p, "test", ir, err)) << err;
    std::string text;
    ASSERT_TRUE(delve::write_ir_v2_json(ir, text, err)) << err;
    // A delve-ir/2 file (no prov keys anywhere) stays readable (N7); chains
    // come out empty.
    auto doc = nlohmann::ordered_json::parse(text);
    doc["format"] = "delve-ir/2";
    for (auto& r : doc.at("rooms")) r.erase("prov");
    for (auto& f : doc.at("facings")) f.erase("prov");
    for (auto& n : doc.at("nodes"))
        for (auto& f : n.at("faces")) f.erase("prov");
    for (auto& d : doc.at("doors")) d.erase("prov");
    for (auto& t : doc.at("transitions")) t.erase("prov");
    delve::IrV2 back;
    ASSERT_TRUE(delve::read_ir_v2_json(doc.dump(), back, err)) << err;
    ASSERT_EQ(back.rooms.size(), ir.rooms.size());
    EXPECT_TRUE(back.rooms[0].prov.empty());
    EXPECT_TRUE(back.facings[0].prov.empty());
    EXPECT_TRUE(back.doors[0].prov.empty());
    // Geometry is unaffected.
    EXPECT_EQ(back.walls.size(), ir.walls.size());
    EXPECT_EQ(back.transitions.size(), ir.transitions.size());
}

TEST(Seeds, Stable31Bit) {
    const int a = delve::unit_seed(7, "wall:1,0-5,0");
    EXPECT_EQ(a, delve::unit_seed(7, "wall:1,0-5,0"));
    EXPECT_NE(a, delve::unit_seed(7, "wall:0,0-0,4"));
    EXPECT_NE(a, delve::unit_seed(8, "wall:1,0-5,0"));
    EXPECT_GE(a, 0);
    const int z = delve::zone_seed(3);
    EXPECT_EQ(z, delve::zone_seed(3));
    EXPECT_NE(z, delve::zone_seed(4));
    EXPECT_GE(z, 0);
}

// --- D2.3b: IR from a generated layout (delve-layout/0) ----------------------

namespace {

// A v1 project: d1 fill params + a programmatic layout tier (F1 validation is
// load_project's business; the IR builder is exercised directly).
delve::Project loadD1AsV1(delve::LayoutParams lp) {
    delve::Project p = loadFixtureProject();
    p.format = delve::kProjectFormatV1;
    p.layout = std::move(lp);
    return p;
}

delve::LayoutParams cornerGraph() {
    delve::LayoutParams lp;
    delve::GraphRoom r0;
    r0.id = "0";
    r0.role = "hall";
    delve::GraphRoom r1;
    r1.id = "1";
    r1.role = "corridor";
    lp.rooms = {r0, r1};
    delve::Passage pass;
    pass.a = "0";
    pass.b = "1";
    pass.door = "open";
    lp.passages = {pass};
    return lp;
}

// corner_frozen.json geometry as delve-layout/0 text (ids stringified, roles
// from the corridor flag, parametric template names).
std::string cornerLayoutText() {
    const auto doc =
        nlohmann::json::parse(readFile(std::string(DELVE_TEST_DATA) + "/corner_frozen.json"));
    nlohmann::ordered_json out;
    out["format"] = "delve-layout/0";
    out["source"] = {{"project", "corner"}, {"seed", 0}};
    nlohmann::ordered_json rooms = nlohmann::ordered_json::array();
    for (const auto& jr : doc["rooms"]) {
        const bool corr = jr["corridor"].get<bool>();
        nlohmann::ordered_json doors = nlohmann::ordered_json::array();
        for (const auto& jd : jr["doors"])
            doors.push_back({{"to", std::to_string(jd["to"].get<int>())}, {"grid", jd["grid"]}});
        rooms.push_back({{"id", std::to_string(jr["id"].get<int>())},
                         {"role", corr ? "corridor" : "hall"},
                         {"template", "param"},
                         {"corridor", corr},
                         {"grid", jr["grid"]},
                         {"doors", std::move(doors)}});
    }
    out["rooms"] = std::move(rooms);
    return out.dump() + "\n";
}

// Two 4x4 rooms side by side (a west, b east), one 2-cell door on x=4.
std::string abLayoutText(const std::string& door_seg = "[[4, 1], [4, 3]]") {
    std::string text = R"({"format": "delve-layout/0", "source": {"project": "t", "seed": 0},
 "rooms": [
  {"id": "a", "role": "hall", "template": "param", "corridor": false,
   "grid": [[0, 0], [4, 0], [4, 4], [0, 4]],
   "doors": [{"to": "b", "grid": @DOOR@}]},
  {"id": "b", "role": "crypt", "template": "param", "corridor": false,
   "grid": [[4, 0], [8, 0], [8, 4], [4, 4]],
   "doors": [{"to": "a", "grid": @DOOR@}]}
 ]}
)";
    for (size_t pos = 0; (pos = text.find("@DOOR@", pos)) != std::string::npos;)
        text.replace(pos, 6, door_seg);
    return text;
}

delve::LayoutParams abGraph(const std::string& door = "gate") {
    delve::LayoutParams lp;
    delve::GraphRoom a;
    a.id = "a";
    a.role = "hall";
    delve::GraphRoom b;
    b.id = "b";
    b.role = "crypt";
    lp.rooms = {a, b};
    delve::Passage pass;
    pass.a = "a";
    pass.b = "b";
    pass.door = door;
    lp.passages = {pass};
    return lp;
}

}  // namespace

TEST(IrFromLayout, MatchesFrozenPath) {
    std::string err;
    delve::Project pv0 = loadFixtureProject();
    const std::string frozen_path = std::string(DELVE_TEST_DATA) + "/corner_frozen.json";
    delve::IrV2 a;
    ASSERT_TRUE(delve::build_ir_v2(readFile(frozen_path), frozen_path, pv0, "test", a, err)) << err;

    delve::Project pv1 = loadD1AsV1(cornerGraph());
    delve::LayoutData ld;
    ASSERT_TRUE(delve::read_layout_json(cornerLayoutText(), ld, err)) << err;
    delve::IrV2 b;
    ASSERT_TRUE(delve::build_ir_from_layout(ld, pv1, "test", b, err)) << err;
    EXPECT_TRUE(b.from_layout);

    std::string ta, tb;
    ASSERT_TRUE(delve::write_ir_v2_json(a, ta, err));
    ASSERT_TRUE(delve::write_ir_v2_json(b, tb, err));
    auto ja = nlohmann::ordered_json::parse(ta);
    auto jb = nlohmann::ordered_json::parse(tb);
    ja.erase("source");  // provenance differs by construction
    jb.erase("source");
    // The dtype chain level also differs by construction: the frozen path has
    // no passage edges (fixed open = default), the layout path cites the
    // passage. Everything else must be byte-identical.
    for (auto& d : ja.at("doors")) d.at("prov").erase("dtype");
    for (auto& d : jb.at("doors")) d.at("prov").erase("dtype");
    EXPECT_EQ(ja.dump(), jb.dump());  // byte-identical IR from both paths
}

TEST(IrFromLayout, DoorTypeLengthAndThickness) {
    delve::Project p = loadD1AsV1(abGraph());
    p.fill.roles["hall"].wall_t = 0.5;  // equal on both sides: legal (5.2)
    p.fill.roles["crypt"].wall_t = 0.5;
    p.fill.roles["hall"].set_fields.insert("wall_t");  // F12: mark explicit (see RoleEntry)
    p.fill.roles["crypt"].set_fields.insert("wall_t");
    delve::LayoutData ld;
    std::string err;
    ASSERT_TRUE(delve::read_layout_json(abLayoutText(), ld, err)) << err;
    delve::IrV2 ir;
    ASSERT_TRUE(delve::build_ir_from_layout(ld, p, "test", ir, err)) << err;

    ASSERT_EQ(ir.doors.size(), 1u);
    EXPECT_EQ(ir.doors[0].id, "door:a-b");
    EXPECT_EQ(ir.doors[0].dtype, 2);  // gate, from the passage edge
    EXPECT_DOUBLE_EQ(ir.doors[0].clear, 4.0 - 2.0 * 0.15);  // 2 cells x 2.0 m
    EXPECT_DOUBLE_EQ(ir.doors[0].thick, 0.5);

    EXPECT_EQ(ir.walls.size(), 7u);  // 6 outer + 1 shared
    EXPECT_EQ(ir.nodes.size(), 6u);
    EXPECT_EQ(ir.facings.size(), 8u);
    for (const auto& w : ir.walls) {
        EXPECT_DOUBLE_EQ(w.thick, 0.5) << w.id;
        EXPECT_DOUBLE_EQ(w.t_end0, 0.5) << w.id;
        EXPECT_DOUBLE_EQ(w.t_end1, 0.5) << w.id;
    }
    // The shared wall's facings carry the full 2-cell cut on both sides.
    const delve::IrFacing* fa = findFacing(ir, "fac:a:1");
    const delve::IrFacing* fb = findFacing(ir, "fac:b:3");
    ASSERT_NE(fa, nullptr);
    ASSERT_NE(fb, nullptr);
    ASSERT_EQ(fa->cuts.size(), 1u);
    ASSERT_EQ(fb->cuts.size(), 1u);
    EXPECT_DOUBLE_EQ(std::hypot(fa->cuts[0].b.first - fa->cuts[0].a.first,
                                fa->cuts[0].b.second - fa->cuts[0].a.second),
                     4.0);
    // Room ids are the graph's; roles come from the graph, not the corridor flag.
    EXPECT_EQ(ir.rooms[0].id, "a");
    EXPECT_EQ(ir.rooms[0].role, "hall");
    EXPECT_EQ(ir.rooms[1].role, "crypt");
    // Styles: hall room base is stone (d1 roles); crypt has no entry -> "*".
    EXPECT_EQ(ir.rooms[0].style, "stone");
    EXPECT_EQ(ir.rooms[1].style, "stone");
    // No corridors -> no derived clears; no style splits -> no transitions.
    EXPECT_TRUE(ir.corridor_clear.empty());
    EXPECT_TRUE(ir.transitions.empty());
}

TEST(IrFromLayout, SharedWallThicknessMismatch) {
    delve::Project p = loadD1AsV1(abGraph());
    p.fill.roles["hall"].wall_t = 0.5;
    p.fill.roles["crypt"].wall_t = 0.7;
    p.fill.roles["hall"].set_fields.insert("wall_t");  // F12: mark explicit (see RoleEntry)
    p.fill.roles["crypt"].set_fields.insert("wall_t");
    delve::LayoutData ld;
    std::string err;
    ASSERT_TRUE(delve::read_layout_json(abLayoutText(), ld, err)) << err;
    delve::IrV2 ir;
    EXPECT_FALSE(delve::build_ir_from_layout(ld, p, "test", ir, err));
    EXPECT_NE(err.find("rooms a and b"), std::string::npos) << err;
    EXPECT_NE(err.find("5.2"), std::string::npos) << err;
    // F12: the error shows where each side's wall_t comes from.
    EXPECT_NE(err.find("a wall_t: 0.5 <- role \"hall\""), std::string::npos) << err;
    EXPECT_NE(err.find("b wall_t: 0.7 <- role \"crypt\""), std::string::npos) << err;
}

TEST(IrFromLayout, FillOverridesReachIR) {
    delve::LayoutParams lp;
    delve::GraphRoom h;
    h.id = "h";
    h.role = "hall";
    h.fill.h = 3.5;  // room beats template/role/project
    lp.rooms = {h};
    delve::TemplateDecl grand;
    grand.name = "grand";
    grand.roles = {"hall"};
    grand.fill.style = "brick";  // template beats role/project
    lp.templates = {grand};
    delve::Project p = loadD1AsV1(lp);
    p.fill.side_rules.clear();  // isolate the base style

    delve::LayoutData ld;
    std::string err;
    ASSERT_TRUE(delve::read_layout_json(
            R"({"format": "delve-layout/0", "source": {"project": "t", "seed": 0},
                "rooms": [{"id": "h", "role": "hall", "template": "grand", "corridor": false,
                           "grid": [[0, 0], [4, 0], [4, 4], [0, 4]], "doors": []}]})",
            ld, err))
        << err;
    delve::IrV2 ir;
    ASSERT_TRUE(delve::build_ir_from_layout(ld, p, "test", ir, err)) << err;
    ASSERT_EQ(ir.rooms.size(), 1u);
    EXPECT_DOUBLE_EQ(ir.rooms[0].h, 3.5);
    EXPECT_EQ(ir.rooms[0].style, "brick");
    ASSERT_FALSE(ir.facings.empty());
    for (const auto& f : ir.facings) EXPECT_EQ(f.style, "brick") << f.id;
    // F12: the chains explain where each value came from.
    const auto& prov = ir.rooms[0].prov;
    ASSERT_EQ(prov.at("h").size(), 2u);  // the template overrides style only
    EXPECT_EQ(prov.at("h")[0].level, "role");
    EXPECT_EQ(prov.at("h")[0].detail, "*");
    EXPECT_EQ(prov.at("h")[1].level, "room");
    EXPECT_EQ(prov.at("h")[1].detail, "h");
    EXPECT_EQ(prov.at("h")[1].value, "3.5");
    EXPECT_EQ(delve::format_prov(prov.at("h")), "3.5 <- room \"h\" <- role \"*\" (3)");
    ASSERT_EQ(prov.at("style").size(), 2u);
    EXPECT_EQ(prov.at("style")[1].level, "template");
    EXPECT_EQ(prov.at("style")[1].detail, "grand");
    EXPECT_EQ(prov.at("style")[1].value, "brick");
    EXPECT_EQ(delve::format_prov(prov.at("style")), "brick <- template \"grand\" <- role \"*\" (stone)");
    // Facings: the base chain without side rules (cleared above).
    ASSERT_FALSE(ir.facings[0].prov.at("style").empty());
    EXPECT_EQ(ir.facings[0].prov.at("style").back().value, "brick");
}

TEST(IrFromLayout, FiguredConcaveCorner) {
    delve::LayoutParams lp;
    delve::GraphRoom l;
    l.id = "l";
    l.role = "hall";
    lp.rooms = {l};
    delve::Project p = loadD1AsV1(lp);
    p.fill.side_rules.clear();
    delve::SideRule outer;
    outer.side = "outer";
    outer.style = "brick";
    p.fill.side_rules.push_back(outer);

    // L-shaped room (notch x in [2,4], y in [2,4]); concave corner at (2,2).
    delve::LayoutData ld;
    std::string err;
    ASSERT_TRUE(delve::read_layout_json(
            R"({"format": "delve-layout/0", "source": {"project": "t", "seed": 0},
                "rooms": [{"id": "l", "role": "hall", "template": "param", "corridor": false,
                           "grid": [[0, 0], [4, 0], [4, 2], [2, 2], [2, 4], [0, 4]],
                           "doors": []}]})",
            ld, err))
        << err;
    delve::IrV2 ir;
    ASSERT_TRUE(delve::build_ir_from_layout(ld, p, "test", ir, err)) << err;

    EXPECT_EQ(ir.walls.size(), 6u);
    EXPECT_EQ(ir.nodes.size(), 6u);
    EXPECT_EQ(ir.facings.size(), 6u);
    EXPECT_TRUE(ir.transitions.empty());  // concave flanks resolve alike (v1 proof)
    EXPECT_TRUE(ir.warnings.empty());

    // Development intervals around the concave corner (s in meters, cell 2.0,
    // pillar half 0.3): edge (2,4)->(2,2) then (2,2)->(4,2).
    const delve::IrFacing* f1 = findFacing(ir, "fac:l:1");
    const delve::IrFacing* f2 = findFacing(ir, "fac:l:2");
    ASSERT_NE(f1, nullptr);
    ASSERT_NE(f2, nullptr);
    EXPECT_DOUBLE_EQ(f1->s0, 4.3);
    EXPECT_DOUBLE_EQ(f1->s1, 7.7);
    EXPECT_DOUBLE_EQ(f2->s0, 8.3);
    EXPECT_DOUBLE_EQ(f2->s1, 11.7);

    // The concave pillar: two open faces look into the room itself, styled
    // from their flanks (brick by the outer rule), not the room base (stone).
    const delve::IrNode* n = findNodeAt(ir, {2, 2});
    ASSERT_NE(n, nullptr);
    ASSERT_EQ(n->faces.size(), 2u);
    for (const auto& f : n->faces) {
        EXPECT_EQ(f.room, "l");
        EXPECT_EQ(f.style, "brick");
        EXPECT_TRUE(f.zones.empty());
    }
    // Normals: west and south (the two open directions at the 270-degree corner).
    std::set<std::pair<double, double>> normals;
    for (const auto& f : n->faces) normals.insert({f.n.first, f.n.second});
    EXPECT_TRUE(normals.count({-1.0, 0.0}));
    EXPECT_TRUE(normals.count({0.0, -1.0}));

    // Stable round-trip with the figured contour.
    std::string t1, t2;
    ASSERT_TRUE(delve::write_ir_v2_json(ir, t1, err));
    delve::IrV2 back;
    ASSERT_TRUE(delve::read_ir_v2_json(t1, back, err)) << err;
    ASSERT_TRUE(delve::write_ir_v2_json(back, t2, err));
    EXPECT_EQ(t1, t2);
}

TEST(IrFromLayout, RejectsBadInput) {
    delve::Project p = loadD1AsV1(abGraph());
    std::string err;
    delve::IrV2 ir;

    // v0 project: no layout tier.
    {
        delve::Project p0 = loadFixtureProject();
        delve::LayoutData ld;
        EXPECT_FALSE(delve::build_ir_from_layout(ld, p0, "test", ir, err));
        EXPECT_NE(err.find("layout tier"), std::string::npos) << err;
    }
    // Layout room not in the graph.
    {
        delve::LayoutData ld;
        ASSERT_TRUE(delve::read_layout_json(
                surgery(abLayoutText(), "\"id\": \"b\"", "\"id\": \"zz\""), ld, err))
            << err;
        EXPECT_FALSE(delve::build_ir_from_layout(ld, p, "test", ir, err));
        EXPECT_NE(err.find("not in the project graph"), std::string::npos) << err;
    }
    // Graph room missing from the layout (same surgery, other direction).
    {
        delve::Project p2 = loadD1AsV1(abGraph());
        delve::GraphRoom extra;
        extra.id = "extra";
        extra.role = "hall";
        p2.layout->rooms.push_back(extra);
        delve::LayoutData ld;
        ASSERT_TRUE(delve::read_layout_json(abLayoutText(), ld, err)) << err;
        EXPECT_FALSE(delve::build_ir_from_layout(ld, p2, "test", ir, err));
        EXPECT_NE(err.find("missing from the layout"), std::string::npos) << err;
    }
    // Role mismatch vs the graph.
    {
        delve::LayoutData ld;
        ASSERT_TRUE(delve::read_layout_json(
                surgery(abLayoutText(), "\"role\": \"crypt\"", "\"role\": \"entry\""), ld, err))
            << err;
        EXPECT_FALSE(delve::build_ir_from_layout(ld, p, "test", ir, err));
        EXPECT_NE(err.find("does not match the graph role"), std::string::npos) << err;
    }
    // Door without a passage.
    {
        delve::Project p3 = loadD1AsV1(abGraph());
        p3.layout->passages.clear();
        delve::LayoutData ld;
        ASSERT_TRUE(delve::read_layout_json(abLayoutText(), ld, err)) << err;
        EXPECT_FALSE(delve::build_ir_from_layout(ld, p3, "test", ir, err));
        EXPECT_NE(err.find("has no passage"), std::string::npos) << err;
    }
    // Unpaired door (b lists a different segment).
    {
        delve::LayoutData ld;
        const std::string text = abLayoutText();
        const std::string from = "\"doors\": [{\"to\": \"a\", \"grid\": [[4, 1], [4, 3]]}]";
        ASSERT_NE(text.find(from), std::string::npos);
        std::string bad = text;
        bad.replace(bad.find(from), from.size(),
                    "\"doors\": [{\"to\": \"a\", \"grid\": [[4, 2], [4, 4]]}]");
        ASSERT_TRUE(delve::read_layout_json(bad, ld, err)) << err;
        EXPECT_FALSE(delve::build_ir_from_layout(ld, p, "test", ir, err));
        EXPECT_NE(err.find("no matching entry"), std::string::npos) << err;
    }
    // Self-intersecting contour (orthogonal bowtie crossing at (2,1)).
    {
        delve::LayoutData ld;
        ASSERT_TRUE(delve::read_layout_json(
                surgery(abLayoutText(), "\"grid\": [[0, 0], [4, 0], [4, 4], [0, 4]]",
                        "\"grid\": [[0, 1], [4, 1], [4, 4], [0, 4], [0, 3], [2, 3], [2, 0], [0, 0]]"),
                ld, err))
            << err;
        EXPECT_FALSE(delve::build_ir_from_layout(ld, p, "test", ir, err));
        EXPECT_NE(err.find("self-intersection"), std::string::npos) << err;
    }
}
