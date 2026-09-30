// Delve D1.2: asset library v1 — R-A4 autonomy (every asset runs from its
// fixture: no E-diagnostics, non-empty mesh, exact anchor counts) + codes
// parity (codes.pgg def-consts vs delve *_code() tables).

#include <gtest/gtest.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pgg/eval.h"
#include "pgg/src/eval/param_text.h"
#include "project.h"

namespace {

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

std::string lower(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

// Fixture JSON value -> param text (slots §5): strings verbatim (@file refs),
// arrays as (x, y, z) vectors, scalars via JSON dump.
std::string jsonToParamText(const nlohmann::json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_array()) {
        std::string t = "(";
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) t += ", ";
            t += v[i].dump();
        }
        return t + ")";
    }
    return v.dump();
}

std::string diagText(const std::vector<pgg::Diagnostic>& ds) {
    std::string t;
    for (const auto& d : ds) {
        t += (d.isWarning ? "[W] " : "[E] ") + d.code + ": " + d.message;
        if (!d.hint.empty()) t += " (" + d.hint + ")";
        t += "\n";
    }
    return t;
}

const pgg::RunOutput* findOutput(const pgg::RunResult& r, const std::string& name) {
    for (const auto& o : r.outputs)
        if (o.name == name) return &o;
    return nullptr;
}

struct AssetCase {
    const char* asset;    // relative to DELVE_ASSETS_DIR
    const char* fixture;  // relative to DELVE_ASSETS_DIR
    size_t anchors;       // exact expected anchor count (frozen fixture)
};

TEST(DelveAssets, AutonomyRA4) {
    const std::string assets = DELVE_ASSETS_DIR;
    const std::vector<AssetCase> cases = {
        {"rooms/fill_v1.pgg", "rooms/fill_v1.fixture.json", 2},
        {"walls/body_v1.pgg", "walls/body_v1.fixture.json", 0},
        {"walls/facing_v1.pgg", "walls/facing_v1.fixture.json", 0},
        {"walls/node_v1.pgg", "walls/node_v1.fixture.json", 0},
        {"doors/opening_v1.pgg", "doors/opening_v1.fixture.json", 0},
        {"decor/lamp_v1.pgg", "decor/lamp_v1.fixture.json", 1},
    };
    for (const AssetCase& c : cases) {
        SCOPED_TRACE(c.asset);
        const std::string fixturePath = assets + "/" + c.fixture;
        const std::string text = readFile(fixturePath);
        ASSERT_FALSE(text.empty()) << "missing " << fixturePath;
        const nlohmann::json fixture = nlohmann::json::parse(text);
        ASSERT_TRUE(fixture.contains("params") && fixture["params"].is_object());

        const std::string fixtureDir =
            std::filesystem::path(fixturePath).parent_path().string();
        pgg::RunParams rp;
        rp.importRoots = {assets};
        for (const auto& [name, value] : fixture["params"].items()) {
            pgg::Value bound;
            std::string err;
            const std::string paramText = jsonToParamText(value);
            ASSERT_TRUE(pgg::parseParamText(paramText, fixtureDir, bound, &err))
                << name << " = " << paramText << ": " << err;
            rp.values.emplace_back(name, bound);
        }
        const pgg::RunResult r = pgg::runFile(assets + "/" + c.asset, rp);
        EXPECT_FALSE(r.hasErrors()) << diagText(r.diagnostics);
        ASSERT_EQ(r.outputs.size(), 2u);

        const pgg::RunOutput* mesh = findOutput(r, "mesh");
        const pgg::RunOutput* anchors = findOutput(r, "anchors");
        ASSERT_NE(mesh, nullptr);
        ASSERT_NE(anchors, nullptr);
        EXPECT_EQ(pgg::asGeo(mesh->value)->kind, pgg::GeoKind::Mesh);
        EXPECT_GT(pgg::asGeo(mesh->value)->pointCount(), 0u);
        EXPECT_EQ(pgg::asGeo(anchors->value)->kind, pgg::GeoKind::Points);
        EXPECT_EQ(pgg::asGeo(anchors->value)->pointCount(), c.anchors);
    }
}

TEST(DelveAssets, CodesParity) {
    // (codes.pgg PREFIX, lowercase name, value, delve table).
    struct Entry {
        const char* prefix;
        const char* name;
        int value;
        int (*code)(const std::string&, bool&);
    };
    const std::vector<Entry> expected = {
        {"ST", "stone", 1, delve::style_code},
        {"ST", "brick", 2, delve::style_code},
        {"ST", "plain", 3, delve::style_code},
        {"ST", "mortar", 4, delve::style_code},
        {"RL", "hall", 1, delve::role_code},
        {"RL", "corridor", 2, delve::role_code},
        {"RL", "crypt", 3, delve::role_code},
        {"RL", "entry", 4, delve::role_code},
        {"RL", "stairs", 5, delve::role_code},
        {"DR", "open", 1, delve::door_code},
        {"DR", "gate", 2, delve::door_code},
        {"DT", "lamp", 1, delve::decor_code},
        {"AK", "light", 1, delve::anchor_code},
        {"AK", "spawn", 2, delve::anchor_code},
        {"AK", "poi", 3, delve::anchor_code},
        {"ZP", "butt", 0, delve::pattern_code},
        {"ZP", "chase", 1, delve::pattern_code},
    };
    const std::string text = readFile(std::string(DELVE_ASSETS_DIR) + "/codes.pgg");
    ASSERT_FALSE(text.empty());
    const std::regex defRe(
        "def ([A-Z]+)_([A-Z]+)\\(\\) -> \\(out: int\\) \\{[\\s\\S]*?out = (\\d+)");
    std::map<std::string, int> pggCodes;  // "PREFIX_name" -> value
    for (std::sregex_iterator it(text.begin(), text.end(), defRe), end; it != end; ++it)
        pggCodes[(*it)[1].str() + "_" + lower((*it)[2].str())] = std::stoi((*it)[3].str());
    EXPECT_EQ(pggCodes.size(), expected.size()) << "codes.pgg gained/lost a def-const";
    for (const Entry& e : expected) {
        SCOPED_TRACE(std::string(e.prefix) + "_" + e.name);
        const auto it = pggCodes.find(std::string(e.prefix) + "_" + e.name);
        ASSERT_NE(it, pggCodes.end()) << "missing in codes.pgg";
        EXPECT_EQ(it->second, e.value) << "codes.pgg value drift";
        bool ok = false;
        const int got = e.code(e.name, ok);
        EXPECT_TRUE(ok) << "missing in delve table";
        EXPECT_EQ(got, e.value) << "delve table value drift";
    }
    // Bogus names are rejected by every table.
    for (const Entry& e : expected) {
        bool ok = true;
        e.code("no_such_" + std::string(e.name), ok);
        EXPECT_FALSE(ok) << e.prefix;
    }
}

}  // namespace
