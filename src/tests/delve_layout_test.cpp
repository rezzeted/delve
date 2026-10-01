// Delve D2.1: F2 catalog (parametric + explicit templates, room descriptions).

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <climits>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "catalog.h"
#include "edgar/generator/grid2d/manual_door_mode_grid2d.hpp"
#include "edgar/generator/grid2d/simple_door_mode_grid2d.hpp"
#include "generate.h"
#include "project.h"

namespace {

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

bool loadText(const std::string& text, delve::Project& p, std::string& err) {
    const std::string path =
        (std::filesystem::path(testing::TempDir()) / "d2_layout_probe.json").string();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    out.close();
    return delve::load_project(path, p, err);
}

std::string fixture() { return readFile(std::string(DELVE_TEST_DATA) + "/d2_project.json"); }

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

const delve::layout::CatalogEntry* findEntry(const delve::layout::Catalog& c, const std::string& name) {
    for (const auto& e : c.entries)
        if (e.name == name) return &e;
    return nullptr;
}

}  // namespace

TEST(Catalog, FixtureSizeAndStats) {
    const delve::Project p = loadFixture();
    delve::layout::Catalog c;
    std::string err;
    ASSERT_TRUE(delve::layout::build_catalog(p, c, err)) << err;
    EXPECT_EQ(c.stats.templates, 7);
    EXPECT_EQ(c.stats.corridors, 2);
    EXPECT_EQ(c.stats.rects, 4);
    EXPECT_EQ(c.stats.explicit_count, 1);
    // Rotations merge on symmetric outlines (pure geometry, no RNG):
    // 2x3, 2x4, 4x5, 5x4 -> 2 instances; 4x4, 5x5 -> 1; grand_hall -> 4.
    EXPECT_EQ(c.stats.instances, 2 + 2 + 1 + 2 + 2 + 1 + 4);
    ASSERT_EQ(c.entries.size(), 7u);
    EXPECT_EQ(c.entries[0].name, "corridor_2x3");
    EXPECT_EQ(c.entries[1].name, "corridor_2x4");
    EXPECT_EQ(c.entries[2].name, "rect_4x4");
    EXPECT_EQ(c.entries[6].name, "grand_hall");
    EXPECT_TRUE(c.entries[0].parametric);
    EXPECT_FALSE(c.entries[6].parametric);
}

TEST(Catalog, ParametricDoorsAndTransforms) {
    const delve::Project p = loadFixture();
    delve::layout::Catalog c;
    std::string err;
    ASSERT_TRUE(delve::layout::build_catalog(p, c, err)) << err;
    const auto* e = findEntry(c, "corridor_2x3");
    ASSERT_NE(e, nullptr);
    const auto* simple =
        dynamic_cast<const edgar::generator::grid2d::SimpleDoorModeGrid2D*>(&e->edgar.doors());
    ASSERT_NE(simple, nullptr);
    EXPECT_EQ(simple->door_length(), 1);
    EXPECT_EQ(simple->corner_distance(), 1);
    EXPECT_EQ(e->edgar.allowed_transformations().size(), 4u);
    EXPECT_EQ(e->roles, std::vector<std::string>{"corridor"});
    const auto* r = findEntry(c, "rect_4x5");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->roles, (std::vector<std::string>{"entry", "hall"}));
}

TEST(Catalog, ExplicitWindingAndDefaults) {
    const delve::Project p = loadFixture();
    delve::layout::Catalog c;
    std::string err;
    ASSERT_TRUE(delve::layout::build_catalog(p, c, err)) << err;
    const auto* e = findEntry(c, "grand_hall");
    ASSERT_NE(e, nullptr);
    // Fixture contour is area2 > 0; the catalog flips it (first point kept).
    const auto& pts = e->edgar.outline().points();
    ASSERT_EQ(pts.size(), 6u);
    EXPECT_EQ(pts[0].x, 0);
    EXPECT_EQ(pts[0].y, 0);
    EXPECT_EQ(pts[1].x, 0);
    EXPECT_EQ(pts[1].y, 3);
    EXPECT_EQ(e->edgar.allowed_transformations().size(), 4u);  // default rotations
    EXPECT_TRUE(e->fill.style.has_value());
    EXPECT_EQ(*e->fill.style, "brick");
}

TEST(Catalog, ExplicitEmptyTransformsIsIdentityOnly) {
    std::string probe = surgery(fixture(), "\"fill\": {\"style\": \"brick\"}",
                                "\"transforms\": [], \"fill\": {\"style\": \"brick\"}");
    delve::Project p;
    std::string err;
    ASSERT_TRUE(loadText(probe, p, err)) << err;
    delve::layout::Catalog c;
    ASSERT_TRUE(delve::layout::build_catalog(p, c, err)) << err;
    const auto* e = findEntry(c, "grand_hall");
    ASSERT_NE(e, nullptr);
    // The port normalizes an empty list to identity-only in the ctor.
    ASSERT_EQ(e->edgar.allowed_transformations().size(), 1u);
    EXPECT_EQ(e->edgar.allowed_transformations()[0],
              edgar::geometry::TransformationGrid2D::Identity);
}

TEST(Catalog, ManualDoors) {
    std::string probe = surgery(fixture(), "\"doors\": \"simple\"",
                                "\"doors\": {\"manual\": [[[2, 0], [4, 0]]]}");
    delve::Project p;
    std::string err;
    ASSERT_TRUE(loadText(probe, p, err)) << err;
    delve::layout::Catalog c;
    ASSERT_TRUE(delve::layout::build_catalog(p, c, err)) << err;
    const auto* e = findEntry(c, "grand_hall");
    ASSERT_NE(e, nullptr);
    const auto* manual =
        dynamic_cast<const edgar::generator::grid2d::ManualDoorModeGrid2D*>(&e->edgar.doors());
    ASSERT_NE(manual, nullptr);
    EXPECT_EQ(manual->doors().size(), 1u);
    EXPECT_EQ(manual->doors()[0].from.x, 2);
    EXPECT_EQ(manual->doors()[0].to.x, 4);
    EXPECT_EQ(manual->doors()[0].socket, nullptr);
}

TEST(Catalog, NameCollisionRejected) {
    std::string probe = surgery(fixture(), "\"name\": \"grand_hall\"", "\"name\": \"rect_4x5\"");
    delve::Project p;
    std::string err;
    ASSERT_TRUE(loadText(probe, p, err)) << err;  // F1 passes (parametric names are derived)
    delve::layout::Catalog c;
    EXPECT_FALSE(delve::layout::build_catalog(p, c, err));
    EXPECT_NE(err.find("collides"), std::string::npos) << err;
}

TEST(Catalog, RoomDescriptions) {
    const delve::Project p = loadFixture();
    delve::layout::Catalog c;
    std::string err;
    ASSERT_TRUE(delve::layout::build_catalog(p, c, err)) << err;
    std::map<std::string, edgar::generator::grid2d::RoomDescriptionGrid2D> desc;
    ASSERT_TRUE(delve::layout::build_room_descriptions(p, c, desc, err)) << err;
    ASSERT_EQ(desc.size(), 3u);
    EXPECT_EQ(desc.at("hall").room_templates().size(), 5u);  // 4 rects + grand_hall
    EXPECT_EQ(desc.at("entry").room_templates().size(), 4u);
    EXPECT_EQ(desc.at("c1").room_templates().size(), 2u);
    EXPECT_TRUE(desc.at("c1").is_corridor());
    EXPECT_FALSE(desc.at("hall").is_corridor());
}

TEST(Catalog, CorridorWidthRegenerates) {
    // F2 acceptance (§7): changing the corridor width in the project changes
    // the parametric corridor templates; explicit templates stay untouched.
    delve::layout::Catalog c2;
    {
        const delve::Project p = loadFixture();
        std::string err;
        ASSERT_TRUE(delve::layout::build_catalog(p, c2, err)) << err;
    }
    delve::layout::Catalog c3;
    {
        delve::Project p;
        std::string err;
        ASSERT_TRUE(loadText(surgery(fixture(), "\"width\": 2, \"length\": [3, 4]",
                                     "\"width\": 3, \"length\": [3, 4]"),
                             p, err))
            << err;
        ASSERT_TRUE(delve::layout::build_catalog(p, c3, err)) << err;
    }
    // Parametric corridors regenerated at the new width (names carry it).
    ASSERT_NE(findEntry(c2, "corridor_2x3"), nullptr);
    ASSERT_NE(findEntry(c2, "corridor_2x4"), nullptr);
    EXPECT_EQ(findEntry(c3, "corridor_2x3"), nullptr);
    EXPECT_EQ(findEntry(c3, "corridor_2x4"), nullptr);
    ASSERT_NE(findEntry(c3, "corridor_3x3"), nullptr);
    ASSERT_NE(findEntry(c3, "corridor_3x4"), nullptr);
    // The width is baked into the contour: outlines must differ.
    const auto outlinesEqual = [](const delve::layout::CatalogEntry& a,
                                  const delve::layout::CatalogEntry& b) {
        const auto& pa = a.edgar.outline().points();
        const auto& pb = b.edgar.outline().points();
        if (pa.size() != pb.size()) return false;
        for (size_t i = 0; i < pa.size(); ++i)
            if (pa[i].x != pb[i].x || pa[i].y != pb[i].y) return false;
        return true;
    };
    EXPECT_FALSE(outlinesEqual(*findEntry(c2, "corridor_2x3"), *findEntry(c3, "corridor_3x3")));
    // Rects and the explicit template are byte-for-byte the same entries.
    for (const char* name : {"rect_4x4", "rect_4x5", "rect_5x4", "rect_5x5", "grand_hall"}) {
        const auto* a = findEntry(c2, name);
        const auto* b = findEntry(c3, name);
        ASSERT_NE(a, nullptr) << name;
        ASSERT_NE(b, nullptr) << name;
        EXPECT_TRUE(outlinesEqual(*a, *b)) << name;
        EXPECT_EQ(a->roles, b->roles) << name;
        EXPECT_EQ(a->edgar.allowed_transformations(), b->edgar.allowed_transformations()) << name;
        EXPECT_EQ(a->fill.style, b->fill.style) << name;
        EXPECT_EQ(a->parametric, b->parametric) << name;
    }
}

TEST(Catalog, V0ProjectRejected) {
    delve::Project p;
    std::string err;
    ASSERT_TRUE(loadText(readFile(std::string(DELVE_TEST_DATA) + "/d1_project.json"), p, err))
        << err;
    delve::layout::Catalog c;
    EXPECT_FALSE(delve::layout::build_catalog(p, c, err));
    EXPECT_NE(err.find("layout"), std::string::npos) << err;
}

namespace {

delve::layout::Catalog loadCatalog(const delve::Project& p) {
    delve::layout::Catalog c;
    std::string err;
    EXPECT_TRUE(delve::layout::build_catalog(p, c, err)) << err;
    return c;
}

delve::Project loadData(const std::string& name) {
    delve::Project p;
    std::string err;
    EXPECT_TRUE(loadText(readFile(std::string(DELVE_TEST_DATA) + "/" + name), p, err)) << err;
    return p;
}

// World-space door segments of a result room: ((to index), (min, max)).
std::vector<std::pair<int, std::pair<delve::CellPt, delve::CellPt>>> roomDoors(
    const edgar::generator::grid2d::LayoutRoomGrid2D<int>& room) {
    std::vector<std::pair<int, std::pair<delve::CellPt, delve::CellPt>>> out;
    for (const auto& d : room.doors) {
        delve::CellPt a{d.door_line.from.x + room.position.x, d.door_line.from.y + room.position.y};
        delve::CellPt b{d.door_line.to.x + room.position.x, d.door_line.to.y + room.position.y};
        out.push_back({d.to_room, {std::min(a, b), std::max(a, b)}});
    }
    return out;
}

}  // namespace

TEST(Generate, SmallLayoutValid) {
    const delve::Project p = loadFixture();
    const delve::layout::Catalog c = loadCatalog(p);
    delve::layout::LayoutGenerator gen;
    delve::layout::LayoutResult r;
    std::string err;
    delve::layout::GenerateOptions opts;
    ASSERT_TRUE(gen.generate(p, c, opts, r, err)) << err;
    EXPECT_EQ(r.layout.rooms.size(), 3u);
    EXPECT_EQ(r.index_to_id.size(), 3u);
    EXPECT_EQ(r.attempt_used, 0);
    EXPECT_EQ(r.seed_used, delve::layout_seed(p.seed));
    // Every passage has a same-segment door on both sides; length == door_length.
    std::map<std::string, int> idx;
    for (size_t i = 0; i < r.index_to_id.size(); ++i) idx[r.index_to_id[i]] = static_cast<int>(i);
    for (const auto& room : r.layout.rooms)
        for (const auto& [to, seg] : roomDoors(room)) {
            const int len = std::abs(seg.first.first - seg.second.first) +
                            std::abs(seg.first.second - seg.second.second);
            EXPECT_EQ(len, 1) << "room " << r.index_to_id[static_cast<size_t>(room.room)];
            // Mirror on the other side.
            bool mirror = false;
            for (const auto& other : r.layout.rooms) {
                if (other.room != to) continue;
                for (const auto& [to2, seg2] : roomDoors(other))
                    if (to2 == room.room && seg2 == seg) mirror = true;
            }
            EXPECT_TRUE(mirror);
        }
    int door_count = 0;
    for (const auto& room : r.layout.rooms) door_count += static_cast<int>(room.doors.size());
    EXPECT_EQ(door_count, 4);  // 2 passages x 2 sides
}

TEST(Generate, DoorLengthReachesLayout) {
    const delve::Project p = loadData("d2_doors.json");
    const delve::layout::Catalog c = loadCatalog(p);
    delve::layout::LayoutGenerator gen;
    delve::layout::LayoutResult r;
    std::string err;
    ASSERT_TRUE(gen.generate(p, c, delve::layout::GenerateOptions{}, r, err)) << err;
    ASSERT_EQ(r.layout.rooms.size(), 2u);
    for (const auto& room : r.layout.rooms) {
        ASSERT_EQ(room.doors.size(), 1u);
        for (const auto& [to, seg] : roomDoors(room))
            EXPECT_EQ(std::abs(seg.first.first - seg.second.first) +
                          std::abs(seg.first.second - seg.second.second),
                      2);
    }
}

TEST(Generate, BadOptionsRejected) {
    const delve::Project p = loadFixture();
    const delve::layout::Catalog c = loadCatalog(p);
    delve::layout::LayoutGenerator gen;
    delve::layout::LayoutResult r;
    std::string err;
    delve::layout::GenerateOptions opts;
    opts.attempts = 0;
    EXPECT_FALSE(gen.generate(p, c, opts, r, err));
    opts.attempts = 1;
    opts.time_budget_ms = -1;
    EXPECT_FALSE(gen.generate(p, c, opts, r, err));
    opts.time_budget_ms.reset();
    opts.iteration_budget = -5;
    EXPECT_FALSE(gen.generate(p, c, opts, r, err));
}

TEST(Generate, ZeroTimeBudgetFailsFast) {
    const delve::Project p = loadFixture();
    const delve::layout::Catalog c = loadCatalog(p);
    delve::layout::LayoutGenerator gen;
    delve::layout::LayoutResult r;
    std::string err;
    delve::layout::GenerateOptions opts;
    opts.attempts = 2;
    opts.time_budget_ms = 0;
    EXPECT_FALSE(gen.generate(p, c, opts, r, err));
    EXPECT_NE(err.find("2 attempt(s)"), std::string::npos) << err;
    EXPECT_NE(err.find("rooms 3"), std::string::npos) << err;
}

TEST(Generate, ZeroIterationBudgetFails) {
    const delve::Project p = loadFixture();
    const delve::layout::Catalog c = loadCatalog(p);
    delve::layout::LayoutGenerator gen;
    delve::layout::LayoutResult r;
    std::string err;
    delve::layout::GenerateOptions opts;
    opts.iteration_budget = 0;
    EXPECT_FALSE(gen.generate(p, c, opts, r, err));
    EXPECT_NE(err.find("iteration budget 0"), std::string::npos) << err;
}

TEST(Generate, CancelBeforeRun) {
    const delve::Project p = loadFixture();
    const delve::layout::Catalog c = loadCatalog(p);
    delve::layout::LayoutGenerator gen;
    delve::layout::LayoutResult r;
    std::string err;
    gen.request_cancel();
    EXPECT_FALSE(gen.generate(p, c, delve::layout::GenerateOptions{}, r, err));
    EXPECT_NE(err.find("cancelled"), std::string::npos) << err;
    gen.reset_cancel();
    EXPECT_TRUE(gen.generate(p, c, delve::layout::GenerateOptions{}, r, err)) << err;
}

TEST(Generate, LayoutJsonRoundTrip) {
    const delve::Project p = loadFixture();
    const delve::layout::Catalog c = loadCatalog(p);
    delve::layout::LayoutGenerator gen;
    delve::layout::LayoutResult r;
    std::string err;
    ASSERT_TRUE(gen.generate(p, c, delve::layout::GenerateOptions{}, r, err)) << err;
    std::string text;
    ASSERT_TRUE(delve::layout::write_layout_json(r, p, "d2_project.json", text, err)) << err;
    // Stable order: rooms sorted by id in the text.
    const size_t c1 = text.find("\"c1\""), entry = text.find("\"entry\""), hall = text.find("\"hall\"");
    EXPECT_TRUE(c1 < entry && entry < hall);
    delve::LayoutData data;
    ASSERT_TRUE(delve::read_layout_json(text, data, err)) << err;
    EXPECT_EQ(data.source_seed, r.seed_used);
    ASSERT_EQ(data.rooms.size(), 3u);
    EXPECT_EQ(data.rooms[0].id, "c1");
    EXPECT_EQ(data.rooms[0].role, "corridor");
    EXPECT_TRUE(data.rooms[0].corridor);
    for (const auto& room : data.rooms) {
        EXPECT_FALSE(room.tmpl.empty());
        EXPECT_LT(delve::contour_area2(room.grid), 0);
        for (const auto& d : room.doors) {
            // Mirror with the same segment on the other side (F4's requirement).
            bool mirror = false;
            for (const auto& other : data.rooms) {
                if (other.id != d.to) continue;
                for (const auto& d2 : other.doors)
                    if (d2.to == room.id && d2.g0 == d.g0 && d2.g1 == d.g1) mirror = true;
            }
            EXPECT_TRUE(mirror) << room.id << " -> " << d.to;
        }
    }
    EXPECT_FALSE(delve::read_layout_json("{\"format\": \"delve-layout/9\"}", data, err));
    EXPECT_NE(err.find("delve-layout/0"), std::string::npos) << err;
}

TEST(Generate, MinDistanceHoldsOnNonNeighbours) {
    // d2_project.json: entry/hall are the non-neighbour pair of the chain.
    // On tree graphs the greedy placer always spreads rooms, so md=0 layouts
    // coincide and no knob-effect seed exists on fast fixtures; this locks the
    // end-to-end contract instead (valid layouts satisfy md, computed here
    // independently of the generator-side recheck).
    const std::string text = readFile(std::string(DELVE_TEST_DATA) + "/d2_project.json");
    ASSERT_FALSE(text.empty());
    for (int md : {1, 2}) {
        for (int seed : {0, 5, 11}) {
            delve::Project pr;
            std::string err;
            ASSERT_TRUE(loadText(text, pr, err)) << err;
            pr.seed = seed;
            pr.layout->min_room_distance = md;
            delve::layout::Catalog c;
            ASSERT_TRUE(delve::layout::build_catalog(pr, c, err)) << err;
            delve::layout::LayoutGenerator gen;
            delve::layout::LayoutResult r;
            delve::layout::GenerateOptions opts;
            opts.attempts = 3;
            ASSERT_TRUE(gen.generate(pr, c, opts, r, err)) << "md=" << md << " seed=" << seed << err;
            std::map<std::string, int> idx;
            for (size_t k = 0; k < r.index_to_id.size(); ++k) idx[r.index_to_id[k]] = (int)k;
            auto bbox = [&](int ri) {
                const edgar::generator::grid2d::LayoutRoomGrid2D<int>* room = nullptr;
                for (const auto& rm : r.layout.rooms)
                    if (rm.room == ri) room = &rm;
                EXPECT_NE(room, nullptr);
                int sx = INT_MAX, bx = INT_MIN, sy = INT_MAX, by = INT_MIN;
                for (const auto& pt : room->outline.points()) {
                    sx = std::min(sx, pt.x + room->position.x);
                    bx = std::max(bx, pt.x + room->position.x);
                    sy = std::min(sy, pt.y + room->position.y);
                    by = std::max(by, pt.y + room->position.y);
                }
                return std::array<int, 4>{sx, bx, sy, by};
            };
            const auto ra = bbox(idx["entry"]), rb = bbox(idx["hall"]);
            const int dx = std::max(0, std::max(ra[0] - rb[1], rb[0] - ra[1]));
            const int dy = std::max(0, std::max(ra[2] - rb[3], rb[2] - ra[3]));
            EXPECT_GE(dx + dy, md) << "md=" << md << " seed=" << seed;
        }
    }
}
