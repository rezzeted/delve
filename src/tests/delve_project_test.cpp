// Delve D2.0: project v1 (layout tier, F1) load + validation + resolution.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "layout.h"
#include "project.h"

namespace {

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

// Load `text` as a project file; false + err on rejection.
bool loadText(const std::string& text, delve::Project& p, std::string& err) {
    const std::string path =
        (std::filesystem::path(testing::TempDir()) / "d2_probe.json").string();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    out.close();
    return delve::load_project(path, p, err);
}

std::string fixture() { return readFile(std::string(DELVE_TEST_DATA) + "/d2_project.json"); }

// Replace the unique `from` substring; fails the test when absent/ambiguous.
std::string surgery(const std::string& text, const std::string& from, const std::string& to) {
    const size_t first = text.find(from);
    EXPECT_NE(first, std::string::npos) << "anchor missing: " << from;
    EXPECT_EQ(text.find(from, first + 1), std::string::npos) << "anchor ambiguous: " << from;
    std::string out = text;
    out.replace(first, from.size(), to);
    return out;
}

delve::Project loadFixture() {
    delve::Project p;
    std::string err;
    EXPECT_TRUE(loadText(fixture(), p, err)) << err;
    return p;
}

}  // namespace

TEST(ProjectV1, ValidFixture) {
    const delve::Project p = loadFixture();
    EXPECT_EQ(p.format, "delve-project/1");
    ASSERT_TRUE(p.layout.has_value());
    const delve::LayoutParams& l = *p.layout;
    EXPECT_EQ(l.rooms.size(), 3u);
    EXPECT_EQ(l.passages.size(), 2u);
    EXPECT_EQ(l.templates.size(), 1u);
    EXPECT_EQ(l.corridor_width, 2);
    EXPECT_EQ(l.min_room_distance, 1);
    // 2 corridor lengths + 2x2 rects + 1 explicit.
    EXPECT_EQ(delve::parametric_corridor_count(l), 2);
    EXPECT_EQ(delve::parametric_rect_count(l), 4);
    EXPECT_EQ(delve::catalog_size(l), 7);
    EXPECT_TRUE(delve::roles_without_template(l).empty());
}

TEST(ProjectV1, V0StillLoads) {
    delve::Project p;
    std::string err;
    ASSERT_TRUE(loadText(readFile(std::string(DELVE_TEST_DATA) + "/d1_project.json"), p, err))
        << err;
    EXPECT_EQ(p.format, "delve-project/0");
    EXPECT_FALSE(p.layout.has_value());
}

TEST(ProjectV1, V0RejectsLayoutTier) {
    const std::string base = readFile(std::string(DELVE_TEST_DATA) + "/d1_project.json");
    ASSERT_FALSE(base.empty());
    delve::Project p;
    std::string err;
    EXPECT_FALSE(loadText(surgery(base, "\"seed\": 7", "\"seed\": 7,\n  \"layout\": {}"), p, err));
    EXPECT_NE(err.find("layout"), std::string::npos) << err;
}

TEST(ProjectV1, V0RejectsRoleWallT) {
    const std::string base = readFile(std::string(DELVE_TEST_DATA) + "/d1_project.json");
    ASSERT_FALSE(base.empty());
    const std::string from = "\"corridor\": {\"h\": 2.6, \"style\": \"brick\"";
    const size_t at = base.find(from);
    ASSERT_NE(at, std::string::npos);
    std::string probe = base;
    probe.replace(at, from.size(),
                  "\"corridor\": {\"h\": 2.6, \"wall_t\": 0.5, \"style\": \"brick\"");
    delve::Project p;
    std::string err;
    EXPECT_FALSE(loadText(probe, p, err));
    EXPECT_NE(err.find("wall_t"), std::string::npos) << err;
}

TEST(ProjectV1, ResolutionPrecedence) {
    const delve::Project p = loadFixture();
    const delve::LayoutParams& l = *p.layout;
    const delve::FillOverride* tmpl = &l.templates[0].fill;  // style brick
    const delve::FillOverride* room = &l.rooms[1].fill;      // h 3.5 (hall)
    const delve::ResolvedFill r = delve::resolve_room_fill(p, "hall", tmpl, room);
    EXPECT_DOUBLE_EQ(r.h, 3.5);             // room beats template/role/project
    EXPECT_EQ(r.style, "brick");            // template beats role/project
    EXPECT_EQ(r.floor, "stone");            // role "*" default
    EXPECT_DOUBLE_EQ(r.wall_t, 0.6);        // project (corridor role value must not leak)
    const delve::ResolvedFill c = delve::resolve_room_fill(p, "corridor", nullptr, nullptr);
    EXPECT_DOUBLE_EQ(c.wall_t, 0.5);  // role level
    EXPECT_DOUBLE_EQ(c.h, 2.6);
    // Room beats template on the same field.
    delve::FillOverride room_style;
    room_style.style = "stone";
    EXPECT_EQ(delve::resolve_room_fill(p, "hall", tmpl, &room_style).style, "stone");
    // Side rules apply over the resolved base.
    EXPECT_EQ(delve::apply_side_rules(p, r.style, false, "corridor"), "brick");
    EXPECT_EQ(delve::apply_side_rules(p, r.style, true, ""), "stone");
    EXPECT_EQ(delve::apply_side_rules(p, r.style, false, "hall"), "brick");  // base kept
}

TEST(ProjectV1, SeedSplit) {
    const int a = delve::layout_seed(11);
    const int b = delve::fill_seed_v1(11);
    EXPECT_GE(a, 0);
    EXPECT_GE(b, 0);
    EXPECT_NE(a, b);
    EXPECT_NE(a, 11);
    EXPECT_EQ(delve::layout_seed(11), a);  // deterministic
    EXPECT_NE(delve::layout_seed(12), a);
}

TEST(ProjectV1, RejectUnknownKeys) {
    const std::string base = fixture();
    delve::Project p;
    std::string err;
    EXPECT_FALSE(loadText(surgery(base, "\"seed\": 11", "\"seed\": 11,\n  \"bogus\": 1"), p, err));
    EXPECT_FALSE(
        loadText(surgery(base, "\"door_length\": 1", "\"door_length\": 1,\n    \"bogus\": 1"), p, err));
    EXPECT_FALSE(loadText(surgery(base, "\"id\": \"c1\"", "\"id\": \"c1\", \"bogus\": 1"), p, err));
}

TEST(ProjectV1, RejectLayoutMissing) {
    delve::Project p;
    std::string err;
    EXPECT_FALSE(loadText("{\"format\": \"delve-project/1\", \"seed\": 1}", p, err));
    EXPECT_NE(err.find("layout"), std::string::npos) << err;
}

TEST(ProjectV1, RejectBadGraph) {
    const std::string base = fixture();
    delve::Project p;
    std::string err;
    // Duplicate room id.
    EXPECT_FALSE(loadText(surgery(base, "\"id\": \"hall\"", "\"id\": \"entry\""), p, err))
        << "dup id accepted";
    // Unknown role.
    EXPECT_FALSE(loadText(surgery(base, "\"role\": \"entry\"", "\"role\": \"throne\""), p, err));
    // Passage to an unknown room.
    EXPECT_FALSE(loadText(surgery(base, "{\"a\": \"entry\", \"b\": \"c1\"",
                                  "{\"a\": \"entry\", \"b\": \"nowhere\""),
                          p, err));
    // Self passage.
    EXPECT_FALSE(
        loadText(surgery(base, "{\"a\": \"c1\", \"b\": \"hall\"", "{\"a\": \"c1\", \"b\": \"c1\""), p, err));
    // Duplicate passage (reversed).
    EXPECT_FALSE(loadText(surgery(base, "{\"a\": \"c1\", \"b\": \"hall\"",
                                  "{\"a\": \"c1\", \"b\": \"entry\""),
                          p, err));
    // Unknown door type.
    EXPECT_FALSE(loadText(surgery(base, "\"door\": \"gate\"", "\"door\": \"portal\""), p, err));
    // Disconnected (drop the second passage) + corridor left with 1 neighbor.
    const std::string no_second = surgery(base, ",\n      {\"a\": \"c1\", \"b\": \"hall\", \"door\": \"gate\"}", "");
    EXPECT_FALSE(loadText(no_second, p, err)) << err;
    // Corridor connected to a corridor.
    EXPECT_FALSE(loadText(surgery(base, "{\"id\": \"entry\", \"role\": \"entry\"}",
                                  "{\"id\": \"entry\", \"role\": \"corridor\"}"),
                          p, err));
}

TEST(ProjectV1, RejectRoleWithoutTemplate) {
    const std::string base = fixture();
    delve::Project p;
    std::string err;
    // Narrow rects to entry only; re-scope the explicit hall template to crypt.
    std::string probe = surgery(base, "\"rooms_rect\": {\"w\": [4, 5], \"h\": [4, 5]}",
                                "\"rooms_rect\": {\"w\": [4, 5], \"h\": [4, 5], \"roles\": [\"entry\"]}");
    probe = surgery(probe, "\"roles\": [\"hall\"]", "\"roles\": [\"crypt\"]");
    EXPECT_FALSE(loadText(probe, p, err)) << "hall without template accepted";
    EXPECT_NE(err.find("hall"), std::string::npos) << err;
}

TEST(ProjectV1, RejectBadContour) {
    const std::string base = fixture();
    const std::string contour = "[[0, 0], [8, 0], [8, 6], [4, 6], [4, 3], [0, 3]]";
    delve::Project p;
    std::string err;
    // Diagonal edge.
    EXPECT_FALSE(loadText(surgery(base, contour, "[[0, 0], [8, 1], [8, 6], [4, 6], [4, 3], [0, 3]]"),
                          p, err));
    // Self-intersecting (bowtie).
    EXPECT_FALSE(
        loadText(surgery(base, contour, "[[0, 0], [8, 0], [8, 6], [0, 6], [0, 3], [8, 3]]"), p, err));
    // Zero area (degenerate spike).
    EXPECT_FALSE(loadText(surgery(base, contour, "[[0, 0], [8, 0], [8, 0], [0, 0]]"), p, err));
    // Too few points.
    EXPECT_FALSE(loadText(surgery(base, contour, "[[0, 0], [8, 0], [8, 6]]"), p, err));
    // Non-integer point.
    EXPECT_FALSE(loadText(surgery(base, contour, "[[0, 0], [8.5, 0], [8, 6], [0, 6]]"), p, err));
}

TEST(ProjectV1, RejectBadDoorsAndTransforms) {
    const std::string base = fixture();
    delve::Project p;
    std::string err;
    // Manual segment off the contour.
    EXPECT_FALSE(loadText(surgery(base, "\"doors\": \"simple\"",
                                  "\"doors\": {\"manual\": [[[100, 100], [101, 100]]]}"),
                          p, err));
    // Manual + simple override mixed.
    EXPECT_FALSE(
        loadText(surgery(base, "\"doors\": \"simple\"",
                         "\"doors\": {\"length\": 2, \"manual\": [[[0, 0], [1, 0]]]}"),
                 p, err));
    // Bad transform name.
    EXPECT_FALSE(loadText(surgery(base, "\"fill\": {\"style\": \"brick\"}",
                                  "\"transforms\": [\"rot45\"], \"fill\": {\"style\": \"brick\"}"),
                          p, err));
    // Zero manual length.
    EXPECT_FALSE(loadText(surgery(base, "\"doors\": \"simple\"",
                                  "\"doors\": {\"manual\": [[[0, 0], [0, 0]]]}"),
                          p, err));
}

TEST(ProjectV1, RejectBudgetExceeded) {
    const std::string base = fixture();
    delve::Project p;
    std::string err;
    EXPECT_FALSE(loadText(surgery(base, "\"catalog_budget\": 64", "\"catalog_budget\": 2"), p, err));
    EXPECT_NE(err.find("budget"), std::string::npos) << err;
}

TEST(ProjectV1, RejectInvariant54) {
    const std::string base = fixture();
    delve::Project p;
    std::string err;
    // Role wall_t >= cell.
    EXPECT_FALSE(
        loadText(surgery(base, "\"wall_t\": 0.5", "\"wall_t\": 2.5"), p, err)) << err;
    // Narrow corridor: clear 2*2-0.6=3.4 stays; force via min_passage.
    EXPECT_FALSE(loadText(surgery(base, "\"min_passage\": 1.2", "\"min_passage\": 9.0"), p, err));
    // Opening <= 0 via a huge frame.
    EXPECT_FALSE(loadText(surgery(base, "\"frame\": 0.15", "\"frame\": 2.0"), p, err));
    // Corner distance 0 reaches the joint.
    EXPECT_FALSE(
        loadText(surgery(base, "\"door_corner_distance\": 1", "\"door_corner_distance\": 0"), p, err));
    // Per-template corner override 0.
    EXPECT_FALSE(loadText(surgery(base, "\"doors\": \"simple\"",
                                  "\"doors\": {\"corner_distance\": 0}"),
                          p, err));
    // Manual door at the corner (endpoint on a contour vertex).
    EXPECT_FALSE(loadText(surgery(base, "\"doors\": \"simple\"",
                                  "\"doors\": {\"manual\": [[[0, 0], [1, 0]]]}"),
                          p, err));
}

// --- F12 provenance (4.2 chains) ----------------------------------------------

TEST(ProjectV1, RoleProvenanceLevels) {
    const delve::Project p = loadFixture();
    // corridor: h 2.6/style brick/floor brick/ceil plain/wall_t 0.5 over "*".
    const delve::RoleProvenance c = delve::resolve_role_prov(p, "corridor");
    ASSERT_EQ(c.prov.at("h").size(), 2u);
    EXPECT_EQ(c.prov.at("h")[0].level, "role");
    EXPECT_EQ(c.prov.at("h")[0].detail, "*");
    EXPECT_EQ(c.prov.at("h")[0].value, "3");
    EXPECT_EQ(c.prov.at("h")[1].detail, "corridor");
    EXPECT_EQ(c.prov.at("h")[1].value, "2.6");
    ASSERT_EQ(c.prov.at("wall_t").size(), 1u);  // "*" has no wall_t
    EXPECT_EQ(c.prov.at("wall_t")[0].detail, "corridor");
    EXPECT_EQ(c.prov.at("wall_t")[0].value, "0.5");
    // entry: only "*" applies; wall_t falls to the project level.
    const delve::RoleProvenance e = delve::resolve_role_prov(p, "entry");
    ASSERT_EQ(e.prov.at("h").size(), 1u);
    EXPECT_EQ(e.prov.at("h")[0].detail, "*");
    ASSERT_EQ(e.prov.at("wall_t").size(), 1u);
    EXPECT_EQ(e.prov.at("wall_t")[0].level, "project");
    EXPECT_EQ(e.prov.at("wall_t")[0].value, "0.6");
    // A role nobody configured resolves from defaults only.
    const delve::RoleProvenance x = delve::resolve_role_prov(p, "crypt");
    ASSERT_EQ(x.prov.at("style").size(), 1u);
    EXPECT_EQ(x.prov.at("style")[0].level, "role");  // "*" sets style
    EXPECT_EQ(x.prov.at("style")[0].detail, "*");
    ASSERT_EQ(x.prov.at("h").size(), 1u);
    EXPECT_EQ(x.prov.at("h")[0].detail, "*");
}

TEST(ProjectV1, RoleProvenanceDefaultsAndProject) {
    delve::Project p;  // hand-built: no roles at all
    p.fill.room_h = 2.9;
    p.fill.wall_t = 0.7;
    const delve::RoleProvenance r = delve::resolve_role_prov(p, "hall");
    ASSERT_EQ(r.prov.at("h").size(), 1u);
    EXPECT_EQ(r.prov.at("h")[0].level, "project");
    EXPECT_EQ(r.prov.at("h")[0].value, "2.9");
    ASSERT_EQ(r.prov.at("wall_t").size(), 1u);
    EXPECT_EQ(r.prov.at("wall_t")[0].level, "project");
    EXPECT_EQ(r.prov.at("wall_t")[0].value, "0.7");
    ASSERT_EQ(r.prov.at("style").size(), 1u);
    EXPECT_EQ(r.prov.at("style")[0].level, "default");
    EXPECT_EQ(r.prov.at("style")[0].value, "stone");
}

TEST(ProjectV1, RoomFillProvenance) {
    const delve::Project p = loadFixture();
    const delve::LayoutParams& l = *p.layout;
    const delve::FillOverride* tmpl = &l.templates[0].fill;  // style brick
    const delve::FillOverride* room = &l.rooms[1].fill;      // h 3.5 (hall)
    const delve::ResolvedFill r =
        delve::resolve_room_fill(p, "hall", tmpl, room, "grand_hall", "hall");
    ASSERT_EQ(r.prov.at("h").size(), 2u);
    EXPECT_EQ(r.prov.at("h")[0].level, "role");
    EXPECT_EQ(r.prov.at("h")[0].detail, "*");
    EXPECT_EQ(r.prov.at("h")[1].level, "room");
    EXPECT_EQ(r.prov.at("h")[1].detail, "hall");
    EXPECT_EQ(r.prov.at("h")[1].value, "3.5");
    ASSERT_EQ(r.prov.at("style").size(), 2u);
    EXPECT_EQ(r.prov.at("style")[1].level, "template");
    EXPECT_EQ(r.prov.at("style")[1].detail, "grand_hall");
    EXPECT_EQ(r.prov.at("style")[1].value, "brick");
    ASSERT_EQ(r.prov.at("floor").size(), 1u);  // untouched by overrides
    EXPECT_EQ(r.prov.at("floor")[0].level, "role");
    // wall_t: no role/project override beyond the project default.
    ASSERT_EQ(r.prov.at("wall_t").size(), 1u);
    EXPECT_EQ(r.prov.at("wall_t")[0].level, "project");
    // Room beats template on the same field, chain records both steps.
    delve::FillOverride room_style;
    room_style.style = "stone";
    const delve::ResolvedFill s =
        delve::resolve_room_fill(p, "hall", tmpl, &room_style, "grand_hall", "hall");
    ASSERT_EQ(s.prov.at("style").size(), 3u);
    EXPECT_EQ(s.prov.at("style")[1].level, "template");
    EXPECT_EQ(s.prov.at("style")[2].level, "room");
    EXPECT_EQ(s.prov.at("style")[2].value, "stone");
}

TEST(ProjectV1, SideRuleProvenance) {
    const delve::Project p = loadFixture();
    // Rules: [0] adjacent_role=corridor -> brick, [1] side=outer -> stone.
    {
        const delve::SideResolution r = delve::apply_side_rules_prov(p, "stone", true, "");
        EXPECT_EQ(r.style, "stone");
        EXPECT_EQ(r.fired, std::vector<int>{1});
    }
    {
        const delve::SideResolution r = delve::apply_side_rules_prov(p, "stone", false, "corridor");
        EXPECT_EQ(r.style, "brick");
        EXPECT_EQ(r.fired, std::vector<int>{0});
    }
    {
        const delve::SideResolution r = delve::apply_side_rules_prov(p, "brick", false, "hall");
        EXPECT_EQ(r.style, "brick");
        EXPECT_TRUE(r.fired.empty());
    }
    // Two matching rules: both fire in order, the later one wins.
    delve::Project q = p;
    delve::SideRule extra;
    extra.side = "outer";
    extra.style = "brick";
    q.fill.side_rules.push_back(extra);  // [2]: outer -> brick (overrides [1])
    const delve::SideResolution r = delve::apply_side_rules_prov(q, "stone", true, "");
    EXPECT_EQ(r.style, "brick");
    EXPECT_EQ(r.fired, (std::vector<int>{1, 2}));
    EXPECT_EQ(delve::side_rule_detail(p, 0), "side_rules[0] (adjacent_role=corridor)");
    EXPECT_EQ(delve::side_rule_detail(p, 1), "side_rules[1] (side=outer)");
}

TEST(ProjectV1, FormatProv) {
    const delve::ProvChain chain = {{"default", "", "3"},
                                    {"role", "hall", "3"},
                                    {"template", "grand_hall", "3.5"},
                                    {"room", "hall", "4"}};
    EXPECT_EQ(delve::format_prov(chain),
              "4 <- room \"hall\" <- template \"grand_hall\" (3.5) <- role \"hall\" (3) <- "
              "default (3)");
    EXPECT_EQ(delve::format_prov({}), "<no provenance>");
    const delve::ProvChain single = {{"project", "", "0.6"}};
    EXPECT_EQ(delve::format_prov(single), "0.6 <- project");
}

TEST(ProjectV1, DecorRulesParse) {
    const std::string base = fixture();
    const std::string text =
        surgery(base, "\"side_rules\": [",
                "\"decor\": [{\"tag\": \"drain\", \"place\": \"floor\", "
                "\"roles\": [\"hall\", \"crypt\"], \"chance\": 0.6, "
                "\"align\": \"near_door\", \"cut_r\": 0.28}, "
                "{\"tag\": \"drain\", \"place\": \"wall\", \"count\": 3, "
                "\"min_dist\": 0.4, \"radius\": 0.35}],\n    "
                "\"side_rules\": [");
    delve::Project p;
    std::string err;
    ASSERT_TRUE(loadText(text, p, err)) << err;
    ASSERT_EQ(p.fill.decor.size(), 2u);
    EXPECT_EQ(p.fill.decor[0].tag, "drain");
    EXPECT_EQ(p.fill.decor[0].place, "floor");
    EXPECT_EQ(p.fill.decor[0].roles, (std::vector<std::string>{"hall", "crypt"}));
    EXPECT_DOUBLE_EQ(p.fill.decor[0].chance, 0.6);
    // v2 defaults keep the v1 behavior; floor-only keys parse on floor rules.
    EXPECT_EQ(p.fill.decor[0].count, 1);
    EXPECT_DOUBLE_EQ(p.fill.decor[0].min_dist, 0.0);
    EXPECT_EQ(p.fill.decor[0].align, "near_door");
    EXPECT_DOUBLE_EQ(p.fill.decor[0].radius, 0.5);
    EXPECT_DOUBLE_EQ(p.fill.decor[0].cut_r, 0.28);
    // v2 fields parse; floor-only keys default on wall rules.
    EXPECT_EQ(p.fill.decor[1].place, "wall");
    EXPECT_EQ(p.fill.decor[1].count, 3);
    EXPECT_DOUBLE_EQ(p.fill.decor[1].min_dist, 0.4);
    EXPECT_EQ(p.fill.decor[1].align, "any");
    EXPECT_DOUBLE_EQ(p.fill.decor[1].radius, 0.35);
    EXPECT_DOUBLE_EQ(p.fill.decor[1].cut_r, 0.0);
    // No decor key at all -> empty rules, defaults intact.
    EXPECT_TRUE(loadFixture().fill.decor.empty());
}

TEST(ProjectV1, DecorRulesReject) {
    const std::string base = fixture();
    const std::string anchor = "\"side_rules\": [";
    const std::pair<const char*, const char*> probes[] = {
        {"{\"tag\": \"lantern\"}", "tag"},                    // unknown decor tag
        {"{\"tag\": \"lamp\"}", "lamp"},                      // lamp has its own keys
        {"{\"tag\": \"drain\", \"place\": \"ceiling\"}", "place"},
        {"{\"tag\": \"drain\", \"chance\": 1.5}", "chance"},
        {"{\"tag\": \"drain\", \"roles\": [\"*\"]}", "roles"},
        {"{\"tag\": \"drain\", \"roles\": [\"attic\"]}", "roles"},
        {"{\"place\": \"floor\"}", "tag"},  // missing required tag
        {"{\"tag\": \"drain\", \"count\": 0}", "count"},
        {"{\"tag\": \"drain\", \"count\": 1.5}", "count"},
        {"{\"tag\": \"drain\", \"min_dist\": -0.1}", "min_dist"},
        {"{\"tag\": \"drain\", \"align\": \"diagonal\"}", "align"},
        {"{\"tag\": \"drain\", \"radius\": 0}", "radius"},
        {"{\"tag\": \"drain\", \"cut_r\": -0.1}", "cut_r"},
        {"{\"tag\": \"drain\", \"place\": \"wall\", \"align\": \"center\"}", "floor-only"},
        {"{\"tag\": \"drain\", \"place\": \"wall\", \"cut_r\": 0.2}", "floor-only"},
        {"{\"tag\": \"drain\", \"bogus\": 1}", "unknown key"},
    };
    for (const auto& [rule, needle] : probes) {
        delve::Project p;
        std::string err;
        EXPECT_FALSE(loadText(
            surgery(base, anchor,
                    std::string("\"decor\": [") + rule + "],\n    " + anchor),
            p, err)) << rule;
        EXPECT_NE(err.find(needle), std::string::npos) << rule << " -> " << err;
    }
}
