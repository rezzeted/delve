// Delve D2.1: F2 catalog (parametric + explicit templates, room descriptions).

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "catalog.h"
#include "edgar/generator/grid2d/manual_door_mode_grid2d.hpp"
#include "edgar/generator/grid2d/simple_door_mode_grid2d.hpp"
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

TEST(Catalog, V0ProjectRejected) {
    delve::Project p;
    std::string err;
    ASSERT_TRUE(loadText(readFile(std::string(DELVE_TEST_DATA) + "/d1_project.json"), p, err))
        << err;
    delve::layout::Catalog c;
    EXPECT_FALSE(delve::layout::build_catalog(p, c, err));
    EXPECT_NE(err.find("layout"), std::string::npos) << err;
}
