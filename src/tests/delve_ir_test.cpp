// Delve D1.1: project side-rules + IR v2 (F4) + delve-ir/2 JSON round-trip.

#include <gtest/gtest.h>

#include <cstdio>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

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
    // F4: every vertex where walls meet = exactly one node.
    std::set<std::pair<int, int>> ends;
    for (const auto& w : ir.walls) {
        ends.insert(w.g0);
        ends.insert(w.g1);
    }
    EXPECT_EQ(ir.nodes.size(), ends.size());
    for (const auto& e : ends) {
        const std::string id =
            "node:" + std::to_string(e.first) + "," + std::to_string(e.second);
        EXPECT_NE(findNode(ir, id), nullptr) << id;
    }
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
    const delve::IrNode* n5 = findNode(ir, "node:5,0");
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
    const delve::IrNode* n5 = findNode(ir, "node:5,0");
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
    EXPECT_DOUBLE_EQ(f0->from.second, -0.4);
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
