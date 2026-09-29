// Delve D0 test (requirements §11 D0, §5.1, §9.2): verifies the committed
// frozen IR without running edgar (platform-stable; generation itself is
// reproducible only on one platform/build per N1).
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "ir_dump.h"
#include "pgg/eval.h"
#include "pgg/src/eval/geo_file.h"
#include "pgg/src/eval/param_text.h"

namespace {

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in) << path;
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

nlohmann::ordered_json loadIr() {
    const std::string text = readFile(std::string(DELVE_D0_DIR) + "/frozen_ir.json");
    return nlohmann::ordered_json::parse(text);
}

TEST(DelveD0, IrSchemaWindingAndMapping) {
    const nlohmann::ordered_json ir = loadIr();
    EXPECT_EQ(ir.value("format", ""), delve::d0::kIrFormat);
    EXPECT_EQ(ir["source"].value("map", ""), "tutorial_corridors.yml");
    const double cell = ir.value("cell", 0.0);
    EXPECT_GT(cell, 0.0);
    ASSERT_TRUE(ir.contains("rooms") && ir["rooms"].is_array());
    ASSERT_FALSE(ir["rooms"].empty());

    std::unordered_set<int> ids;
    for (const auto& jr : ir["rooms"]) {
        const int id = jr.value("id", -1);
        EXPECT_TRUE(ids.insert(id).second) << "duplicate room " << id;
        ASSERT_TRUE(jr.contains("grid") && jr.contains("contour"));
        EXPECT_EQ(jr["grid"].size(), jr["contour"].size());
        ASSERT_GE(jr["grid"].size(), 3u);

        // §5.1: the stored mapping, exact.
        for (size_t i = 0; i < jr["grid"].size(); ++i) {
            const int gx = jr["grid"][i][0].get<int>();
            const int gy = jr["grid"][i][1].get<int>();
            EXPECT_DOUBLE_EQ(jr["contour"][i][0].get<double>(), delve::d0::grid_to_x(gx, cell));
            EXPECT_DOUBLE_EQ(jr["contour"][i][1].get<double>(), delve::d0::grid_to_z(gy, cell));
        }
        // §5.1: CCW seen from +Y (area2 < 0 in (x, z); identity mapping keeps the sign).
        std::vector<std::pair<int, int>> grid;
        for (const auto& pt : jr["grid"]) grid.push_back({pt[0].get<int>(), pt[1].get<int>()});
        EXPECT_LT(delve::d0::contour_area2(grid), 0) << "room " << id;
    }
    for (const auto& jr : ir["rooms"]) {
        for (const auto& jd : jr["doors"]) {
            const int to = jd.value("to", -1);
            EXPECT_TRUE(ids.count(to)) << "door to unknown room " << to;
            EXPECT_EQ(jd["grid"].size(), 2u);
            EXPECT_EQ(jd["segment"].size(), 2u);
            for (size_t k = 0; k < 2; ++k) {
                const int gx = jd["grid"][k][0].get<int>();
                const int gy = jd["grid"][k][1].get<int>();
                EXPECT_DOUBLE_EQ(jd["segment"][k][0].get<double>(), delve::d0::grid_to_x(gx, cell));
                EXPECT_DOUBLE_EQ(jd["segment"][k][1].get<double>(), delve::d0::grid_to_z(gy, cell));
            }
        }
    }
}

TEST(DelveD0, PointsFileConsistentWithIr) {
    const nlohmann::ordered_json ir = loadIr();
    std::unordered_map<int, bool> corridor_by_room;
    for (const auto& jr : ir["rooms"]) corridor_by_room[jr.value("id", -1)] = jr.value("corridor", false);

    pgg::GeoPtr geo;
    std::string err;
    ASSERT_TRUE(pgg::loadPointsGeo(std::string(DELVE_D0_DIR) + "/rooms.points.json", geo, &err)) << err;
    ASSERT_NE(geo, nullptr);
    EXPECT_GT(geo->pointCount(), 0u);
    ASSERT_NE(geo->pointAttrs, nullptr);
    const pgg::AttrColumn* room_col = geo->pointAttrs->find("room");
    const pgg::AttrColumn* corr_col = geo->pointAttrs->find("corridor");
    ASSERT_NE(room_col, nullptr);
    ASSERT_NE(corr_col, nullptr);
    const auto& rooms = *std::get<std::shared_ptr<const std::vector<int64_t>>>(room_col->data);
    const auto& corrs = *std::get<std::shared_ptr<const std::vector<uint8_t>>>(corr_col->data);
    ASSERT_EQ(rooms.size(), geo->pointCount());
    ASSERT_EQ(corrs.size(), geo->pointCount());
    for (size_t i = 0; i < rooms.size(); ++i) {
        const int room = static_cast<int>(rooms[i]);
        ASSERT_TRUE(corridor_by_room.count(room)) << "point " << i << " room " << room;
        EXPECT_EQ(corrs[i] != 0, corridor_by_room[room]) << "point " << i;
    }
    // @P lies on the floor plane (y = 0, §5.5).
    for (const glm::vec3& p : *geo->positions) EXPECT_DOUBLE_EQ(p.y, 0.0);
}

TEST(DelveD0, GeoPointsRuntimeBinding) {
    // §9.2: the host binds file-loaded geometry into a geo<points> param.
    pgg::Value bound;
    std::string err;
    ASSERT_TRUE(pgg::parseParamText("@rooms.points.json", DELVE_D0_DIR, bound, &err)) << err;
    EXPECT_EQ(pgg::valueBase(bound), pgg::ScalarType::Geo);
    const size_t count = pgg::asGeo(bound)->pointCount();

    pgg::RunParams rp;
    rp.values.push_back({"pts", std::move(bound)});
    const pgg::RunResult r = pgg::run("param pts: geo<points>\nn = count(pts)\noutput n\n", rp);
    for (const pgg::Diagnostic& d : r.diagnostics)
        if (!d.isWarning) ADD_FAILURE() << d.code << " " << d.message;
    ASSERT_FALSE(r.hasErrors());
    ASSERT_EQ(r.outputs.size(), 1u);
    EXPECT_EQ(pgg::asInt(r.outputs[0].value), static_cast<int64_t>(count));
}

TEST(DelveD0, ViewRegenIsStable) {
    const std::string ir_text = readFile(std::string(DELVE_D0_DIR) + "/frozen_ir.json");
    std::string regen;
    std::string err;
    ASSERT_TRUE(delve::d0::render_view_pgg(ir_text, regen, err)) << err;
    EXPECT_EQ(regen, readFile(std::string(DELVE_D0_DIR) + "/d0_view.pgg"));
}

}  // namespace
