// Delve topo view model (delve::topo): projection of the level graph and
// the generated layout into GUI-free view data (the DelveViewer Topo tab
// input). Pure projection — no edgar, no PGG.

#include <gtest/gtest.h>

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

#include "topo.h"

namespace {

using delve::CellPt;

// --- fixtures -----------------------------------------------------------------

// Graph: crypt --gate-- c1 --open-- hall (c1 is a corridor).
delve::LayoutParams graph3() {
    delve::LayoutParams g;
    g.rooms = {{"crypt", "crypt", {"deep"}, {}},
               {"hall", "hall", {"treasure"}, {}},
               {"c1", "corridor", {}, {}}};
    g.passages = {{"hall", "c1", "open"}, {"c1", "crypt", "gate"}};
    return g;
}

// Layout of the graph3 rooms in world grid cells:
//   hall: 4x3 rect (0,0)-(4,3), centroid (2, 1.5)
//   c1:   corridor 2x2 (4,1)-(6,3), centroid (5, 2)
//   crypt: L (6,3)-(10,5) x (6,7)-(8,7) shifted, centroid (23/3, 14/3)
delve::LayoutData layout3() {
    delve::LayoutData d;
    d.source_project = "topo_test";
    d.source_seed = 1;
    delve::LayoutRoomData c1;
    c1.id = "c1";
    c1.role = "corridor";
    c1.tmpl = "corridor_2x2";
    c1.corridor = true;
    c1.grid = {{4, 1}, {6, 1}, {6, 3}, {4, 3}};
    c1.doors = {{"hall", {4, 1}, {4, 2}}, {"crypt", {6, 2}, {6, 3}}};
    delve::LayoutRoomData crypt;
    crypt.id = "crypt";
    crypt.role = "crypt";
    crypt.tmpl = "crypt_L";
    crypt.grid = {{6, 3}, {10, 3}, {10, 5}, {8, 5}, {8, 7}, {6, 7}};
    crypt.doors = {{"c1", {6, 3}, {6, 4}}};
    delve::LayoutRoomData hall;
    hall.id = "hall";
    hall.role = "hall";
    hall.tmpl = "rect_4x3";
    hall.grid = {{0, 0}, {4, 0}, {4, 3}, {0, 3}};
    hall.doors = {{"c1", {4, 1}, {4, 2}}};
    d.rooms = {crypt, c1, hall};  // deliberately not sorted
    return d;
}

// --- helpers --------------------------------------------------------------------

const delve::TopoNode* nodeOf(const delve::TopoModel& m, const std::string& id) {
    for (const auto& n : m.nodes)
        if (n.id == id) return &n;
    return nullptr;
}

std::string dumpModel(const delve::TopoModel& m) {
    std::ostringstream s;
    for (const auto& n : m.nodes) {
        s << "N " << n.id << " role=" << n.role << " tags=";
        for (const auto& t : n.tags) s << t << ",";
        s << " hasLayout=" << n.hasLayout << " corridor=" << n.corridor << " tmpl=" << n.tmpl
          << " c=" << n.cx << "," << n.cz << " bbox=" << n.minx << "," << n.maxx << "," << n.miny
          << "," << n.maxy << " contour=";
        for (const auto& p : n.contour) s << p.first << "," << p.second << ";";
        s << "\n";
    }
    for (const auto& e : m.edges)
        s << "E " << e.a << "->" << e.b << " door=" << e.door << " labelOk=" << e.labelOk
          << " label=" << e.lx << "," << e.lz << "\n";
    for (const auto& t : m.doors)
        s << "D " << t.room << "->" << t.to << " (" << t.g0.first << "," << t.g0.second << ")-("
          << t.g1.first << "," << t.g1.second << ") door=" << t.door << "\n";
    for (const auto& r : m.roles) s << "R " << r << "\n";
    return s.str();
}

}  // namespace

// --- projection -------------------------------------------------------------------

TEST(TopoModel, BuildBasic) {
    const delve::TopoModel m = delve::build_topo(graph3(), layout3());

    // Nodes: sorted by id, graph metadata + placed geometry.
    ASSERT_EQ(m.nodes.size(), 3u);
    EXPECT_EQ(m.nodes[0].id, "c1");
    EXPECT_EQ(m.nodes[1].id, "crypt");
    EXPECT_EQ(m.nodes[2].id, "hall");
    const auto& c1 = m.nodes[0];
    EXPECT_EQ(c1.role, "corridor");
    EXPECT_TRUE(c1.hasLayout);
    EXPECT_TRUE(c1.corridor);
    EXPECT_EQ(c1.tmpl, "corridor_2x2");
    EXPECT_DOUBLE_EQ(c1.cx, 5.0);
    EXPECT_DOUBLE_EQ(c1.cz, 2.0);
    EXPECT_EQ((std::vector<int>{c1.minx, c1.maxx, c1.miny, c1.maxy}), (std::vector<int>{4, 6, 1, 3}));
    EXPECT_EQ(c1.contour, (std::vector<CellPt>{{4, 1}, {6, 1}, {6, 3}, {4, 3}}));
    const auto& crypt = m.nodes[1];
    EXPECT_EQ(crypt.role, "crypt");
    ASSERT_EQ(crypt.tags.size(), 1u);
    EXPECT_EQ(crypt.tags[0], "deep");
    // L-shaped centroid (hand-computed, see the fixture comment).
    EXPECT_DOUBLE_EQ(crypt.cx, 23.0 / 3.0);
    EXPECT_DOUBLE_EQ(crypt.cz, 14.0 / 3.0);
    EXPECT_EQ((std::vector<int>{crypt.minx, crypt.maxx, crypt.miny, crypt.maxy}),
              (std::vector<int>{6, 10, 3, 7}));
    const auto& hall = m.nodes[2];
    EXPECT_EQ(hall.role, "hall");
    ASSERT_EQ(hall.tags.size(), 1u);
    EXPECT_EQ(hall.tags[0], "treasure");
    EXPECT_FALSE(hall.corridor);
    EXPECT_DOUBLE_EQ(hall.cx, 2.0);
    EXPECT_DOUBLE_EQ(hall.cz, 1.5);

    // Edges: sorted by (a, b), door types from the graph, label = midpoint
    // of the node centroids.
    ASSERT_EQ(m.edges.size(), 2u);
    EXPECT_EQ(m.edges[0].a, "c1");
    EXPECT_EQ(m.edges[0].b, "crypt");
    EXPECT_EQ(m.edges[0].door, "gate");
    EXPECT_TRUE(m.edges[0].labelOk);
    EXPECT_DOUBLE_EQ(m.edges[0].lx, (5.0 + 23.0 / 3.0) / 2.0);
    EXPECT_DOUBLE_EQ(m.edges[0].lz, (2.0 + 14.0 / 3.0) / 2.0);
    EXPECT_EQ(m.edges[1].a, "hall");
    EXPECT_EQ(m.edges[1].b, "c1");
    EXPECT_EQ(m.edges[1].door, "open");
    EXPECT_TRUE(m.edges[1].labelOk);
    EXPECT_DOUBLE_EQ(m.edges[1].lx, 3.5);
    EXPECT_DOUBLE_EQ(m.edges[1].lz, 1.75);

    // Doors: sorted by (room, to, g0, g1), dtype resolved by the graph pair.
    ASSERT_EQ(m.doors.size(), 4u);
    EXPECT_EQ((std::vector<std::string>{m.doors[0].room, m.doors[0].to}),
              (std::vector<std::string>{"c1", "crypt"}));
    EXPECT_EQ(m.doors[0].door, "gate");
    EXPECT_EQ((std::vector<std::string>{m.doors[1].room, m.doors[1].to}),
              (std::vector<std::string>{"c1", "hall"}));
    EXPECT_EQ(m.doors[1].door, "open");
    EXPECT_EQ((std::vector<std::string>{m.doors[2].room, m.doors[2].to}),
              (std::vector<std::string>{"crypt", "c1"}));
    EXPECT_EQ(m.doors[2].door, "gate");
    EXPECT_EQ(m.doors[2].g0, (CellPt{6, 3}));
    EXPECT_EQ(m.doors[2].g1, (CellPt{6, 4}));
    EXPECT_EQ((std::vector<std::string>{m.doors[3].room, m.doors[3].to}),
              (std::vector<std::string>{"hall", "c1"}));
    EXPECT_EQ(m.doors[3].door, "open");

    // Palette roles: distinct sorted graph roles.
    EXPECT_EQ(m.roles, (std::vector<std::string>{"corridor", "crypt", "hall"}));
}

TEST(TopoModel, UnplacedAndGhost) {
    auto g = graph3();
    g.rooms.push_back({"bonus", "hall", {}, {}});  // not in the layout
    auto d = layout3();
    delve::LayoutRoomData ghostRoom;  // not in the graph
    ghostRoom.id = "ghost";
    ghostRoom.role = "hall";
    ghostRoom.grid = {{20, 0}, {22, 0}, {22, 2}, {20, 2}};
    d.rooms.push_back(ghostRoom);

    const delve::TopoModel m = delve::build_topo(g, d);
    ASSERT_EQ(m.nodes.size(), 5u);
    const auto* bonus = nodeOf(m, "bonus");
    ASSERT_NE(bonus, nullptr);
    EXPECT_EQ(bonus->role, "hall");
    EXPECT_FALSE(bonus->hasLayout);
    EXPECT_TRUE(bonus->contour.empty());
    EXPECT_EQ(bonus->cx, 0.0);
    const auto* ghost = nodeOf(m, "ghost");
    ASSERT_NE(ghost, nullptr);
    EXPECT_TRUE(ghost->hasLayout);
    EXPECT_TRUE(ghost->role.empty());
    EXPECT_DOUBLE_EQ(ghost->cx, 21.0);
    EXPECT_DOUBLE_EQ(ghost->cz, 1.0);

    // No edge touches bonus/ghost here (the graph has no such passages);
    // add one through a synthetic graph to cover labelOk = false.
    auto g2 = g;
    g2.passages.push_back({"bonus", "hall", "open"});
    const delve::TopoModel m2 = delve::build_topo(g2, d);
    bool found = false;
    for (const auto& e : m2.edges)
        if (e.a == "bonus" && e.b == "hall") {
            found = true;
            EXPECT_FALSE(e.labelOk);
            EXPECT_EQ(e.door, "open");
        }
    EXPECT_TRUE(found);
}

TEST(TopoModel, DeterministicOnInputOrder) {
    auto g = graph3();
    auto d = layout3();
    const std::string ref = dumpModel(delve::build_topo(g, d));

    // Reverse every input order: the projection must be byte-identical.
    std::reverse(g.rooms.begin(), g.rooms.end());
    std::reverse(g.passages.begin(), g.passages.end());
    std::reverse(d.rooms.begin(), d.rooms.end());
    for (auto& r : d.rooms) std::reverse(r.doors.begin(), r.doors.end());
    EXPECT_EQ(dumpModel(delve::build_topo(g, d)), ref);
}

// --- primitives -------------------------------------------------------------------

TEST(TopoCentroid, Rectangle) {
    const auto c = delve::contour_centroid({{1, 2}, {5, 2}, {5, 4}, {1, 4}});
    EXPECT_TRUE(c.ok);
    EXPECT_DOUBLE_EQ(c.x, 3.0);
    EXPECT_DOUBLE_EQ(c.z, 3.0);
}

TEST(TopoCentroid, LShape) {
    // 4x2 rect + 2x2 rect on top-left: hand-computed (5/3, 5/3).
    const auto c =
        delve::contour_centroid({{0, 0}, {4, 0}, {4, 2}, {2, 2}, {2, 4}, {0, 4}});
    EXPECT_TRUE(c.ok);
    EXPECT_DOUBLE_EQ(c.x, 5.0 / 3.0);
    EXPECT_DOUBLE_EQ(c.z, 5.0 / 3.0);
}

TEST(TopoCentroid, WindingAgnostic) {
    const std::vector<CellPt> cw = {{0, 0}, {4, 0}, {4, 3}, {0, 3}};
    std::vector<CellPt> ccw = cw;
    std::reverse(ccw.begin(), ccw.end());
    const auto a = delve::contour_centroid(cw);
    const auto b = delve::contour_centroid(ccw);
    EXPECT_TRUE(a.ok);
    EXPECT_TRUE(b.ok);
    EXPECT_DOUBLE_EQ(a.x, b.x);
    EXPECT_DOUBLE_EQ(a.z, b.z);
    EXPECT_DOUBLE_EQ(a.x, 2.0);
    EXPECT_DOUBLE_EQ(a.z, 1.5);
}

TEST(TopoCentroid, DegenerateFallsBackToBboxMidpoint) {
    // A "line" contour: zero area.
    const auto c = delve::contour_centroid({{0, 0}, {4, 0}, {4, 0}, {0, 0}});
    EXPECT_FALSE(c.ok);
    EXPECT_DOUBLE_EQ(c.x, 2.0);
    EXPECT_DOUBLE_EQ(c.z, 0.0);
    const auto e = delve::contour_centroid({});
    EXPECT_FALSE(e.ok);
    EXPECT_DOUBLE_EQ(e.x, 0.0);
    EXPECT_DOUBLE_EQ(e.z, 0.0);
}

TEST(TopoGridBbox, Basic) {
    int minx = 0, maxx = 0, miny = 0, maxy = 0;
    delve::grid_bbox({{3, -2}, {-1, 5}, {3, 5}}, minx, maxx, miny, maxy);
    EXPECT_EQ((std::vector<int>{minx, maxx, miny, maxy}), (std::vector<int>{-1, 3, -2, 5}));
    delve::grid_bbox({}, minx, maxx, miny, maxy);
    EXPECT_EQ((std::vector<int>{minx, maxx, miny, maxy}), (std::vector<int>{0, 0, 0, 0}));
}

TEST(TopoPassageDoor, BothDirectionsAndMiss) {
    const delve::LayoutParams g = graph3();
    EXPECT_EQ(delve::passage_door(g, "hall", "c1"), "open");
    EXPECT_EQ(delve::passage_door(g, "c1", "hall"), "open");
    EXPECT_EQ(delve::passage_door(g, "crypt", "c1"), "gate");
    EXPECT_EQ(delve::passage_door(g, "c1", "crypt"), "gate");
    EXPECT_TRUE(delve::passage_door(g, "hall", "crypt").empty());
}
