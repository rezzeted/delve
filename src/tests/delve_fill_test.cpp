// Delve D1.3: R-A3 slot checks (lib assets pass, broken assets fail with
// delve/slot errors naming slot/asset/param/expectation/fact).

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "fill.h"

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

}  // namespace
