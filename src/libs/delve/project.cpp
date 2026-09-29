#include "pch.h"

#include "project.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace delve {
namespace {

namespace fs = std::filesystem;

bool is_role_name(const std::string& name) {
    return name == "*" || name == "hall" || name == "corridor" || name == "crypt" ||
           name == "entry" || name == "stairs";
}

bool is_slot_kind(const std::string& kind) {
    if (kind == "room_fill" || kind == "wall_body" || kind == "facing" || kind == "node" ||
        kind == "door")
        return true;
    return kind.rfind("decor:", 0) == 0 && kind.size() > 6;
}

bool get_num(const nlohmann::json& j, const std::string& key, double& out, std::string& err,
             const std::string& where) {
    const auto it = j.find(key);
    if (it == j.end()) return true;  // absent = default
    if (!it->is_number()) {
        err = where + "." + key + ": expected a number";
        return false;
    }
    out = it->get<double>();
    return true;
}

bool get_str(const nlohmann::json& j, const std::string& key, std::string& out, std::string& err,
             const std::string& where) {
    const auto it = j.find(key);
    if (it == j.end()) return true;
    if (!it->is_string()) {
        err = where + "." + key + ": expected a string";
        return false;
    }
    out = it->get<std::string>();
    return true;
}

}  // namespace

bool load_project(const std::string& path, Project& out, std::string& err) {
    Project p;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "cannot open " + path;
        return false;
    }
    std::ostringstream text;
    text << in.rdbuf();
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(text.str());
    } catch (const std::exception& e) {
        err = std::string(path) + ": invalid JSON: " + e.what();
        return false;
    }
    if (!doc.is_object()) {
        err = path + ": expected a JSON object";
        return false;
    }
    for (const auto& [key, _] : doc.items()) {
        if (key != "format" && key != "seed" && key != "fill" && key != "slots" &&
            key != "asset_roots") {
            err = path + ": unknown key \"" + key + "\"";
            return false;
        }
    }
    if (doc.value("format", std::string{}) != kProjectFormat) {
        err = path + ": unsupported format, expected \"" + kProjectFormat + "\"";
        return false;
    }
    if (doc.contains("seed")) {
        if (!doc["seed"].is_number_integer()) {
            err = path + ": seed: expected an integer";
            return false;
        }
        p.seed = doc["seed"].get<int>();
    }
    if (doc.contains("fill")) {
        const auto& f = doc["fill"];
        if (!f.is_object()) {
            err = path + ": fill: expected an object";
            return false;
        }
        for (const auto& [key, _] : f.items()) {
            if (key != "cell" && key != "wall_t" && key != "min_passage" && key != "min_opening" &&
                key != "room_h" && key != "door_h" && key != "frame" && key != "lamp_step" &&
                key != "row_module" && key != "roles" && key != "transitions" &&
                key != "side_rules") {
                err = path + ": fill." + key + ": unknown key";
                return false;
            }
        }
        FillParams& fp = p.fill;
        if (!get_num(f, "cell", fp.cell, err, "fill") || !get_num(f, "wall_t", fp.wall_t, err, "fill") ||
            !get_num(f, "min_passage", fp.min_passage, err, "fill") ||
            !get_num(f, "min_opening", fp.min_opening, err, "fill") ||
            !get_num(f, "room_h", fp.room_h, err, "fill") ||
            !get_num(f, "door_h", fp.door_h, err, "fill") ||
            !get_num(f, "frame", fp.frame, err, "fill") ||
            !get_num(f, "lamp_step", fp.lamp_step, err, "fill") ||
            !get_num(f, "row_module", fp.row_module, err, "fill"))
            return false;
        if (f.contains("roles")) {
            const auto& roles = f["roles"];
            if (!roles.is_object()) {
                err = path + ": fill.roles: expected an object";
                return false;
            }
            for (const auto& [name, entry] : roles.items()) {
                if (!is_role_name(name)) {
                    err = path + ": fill.roles." + name + ": unknown role (hall/corridor/crypt/entry/stairs/*)";
                    return false;
                }
                if (!entry.is_object()) {
                    err = path + ": fill.roles." + name + ": expected an object";
                    return false;
                }
                RoleEntry re;
                // Start from "*" when present, else defaults.
                if (name != "*" && roles.contains("*") && roles["*"].is_object()) {
                    const auto& star = roles["*"];
                    get_num(star, "h", re.h, err, "fill.roles.*");
                    get_str(star, "style", re.style, err, "fill.roles.*");
                    get_str(star, "floor", re.floor, err, "fill.roles.*");
                    get_str(star, "ceil", re.ceil, err, "fill.roles.*");
                }
                for (const auto& [key, _] : entry.items()) {
                    if (key != "h" && key != "style" && key != "floor" && key != "ceil") {
                        err = path + ": fill.roles." + name + "." + key + ": unknown key";
                        return false;
                    }
                }
                const std::string where = "fill.roles." + name;
                if (!get_num(entry, "h", re.h, err, where) ||
                    !get_str(entry, "style", re.style, err, where) ||
                    !get_str(entry, "floor", re.floor, err, where) ||
                    !get_str(entry, "ceil", re.ceil, err, where))
                    return false;
                bool ok = false;
                style_code(re.style, ok);
                if (!ok) {
                    err = path + ": " + where + ".style: unknown style \"" + re.style + "\"";
                    return false;
                }
                style_code(re.floor, ok);
                if (!ok) {
                    err = path + ": " + where + ".floor: unknown style \"" + re.floor + "\"";
                    return false;
                }
                style_code(re.ceil, ok);
                if (!ok) {
                    err = path + ": " + where + ".ceil: unknown style \"" + re.ceil + "\"";
                    return false;
                }
                fp.roles[name] = std::move(re);
            }
        }
        if (f.contains("transitions")) {
            const auto& t = f["transitions"];
            if (!t.is_object()) {
                err = path + ": fill.transitions: expected an object";
                return false;
            }
            for (const auto& [key, _] : t.items()) {
                if (key != "pattern" && key != "width" && key != "place") {
                    err = path + ": fill.transitions." + key + ": unknown key";
                    return false;
                }
            }
            if (!get_str(t, "pattern", fp.transitions.pattern, err, "fill.transitions") ||
                !get_num(t, "width", fp.transitions.width, err, "fill.transitions") ||
                !get_str(t, "place", fp.transitions.place, err, "fill.transitions"))
                return false;
            bool ok = false;
            pattern_code(fp.transitions.pattern, ok);
            if (!ok) {
                err = path + ": fill.transitions.pattern: unknown pattern \"" + fp.transitions.pattern +
                      "\" (butt/chase)";
                return false;
            }
            if (fp.transitions.place != "corner" && fp.transitions.place != "wall") {
                err = path + ": fill.transitions.place: expected corner|wall";
                return false;
            }
        }
        if (f.contains("side_rules")) {
            const auto& rules = f["side_rules"];
            if (!rules.is_array()) {
                err = path + ": fill.side_rules: expected an array";
                return false;
            }
            for (size_t i = 0; i < rules.size(); ++i) {
                const std::string where = "fill.side_rules[" + std::to_string(i) + "]";
                const auto& r = rules[i];
                if (!r.is_object()) {
                    err = path + ": " + where + ": expected an object";
                    return false;
                }
                for (const auto& [key, _] : r.items()) {
                    if (key != "match" && key != "style") {
                        err = path + ": " + where + "." + key + ": unknown key";
                        return false;
                    }
                }
                SideRule rule;
                if (r.contains("match")) {
                    const auto& m = r["match"];
                    if (!m.is_object()) {
                        err = path + ": " + where + ".match: expected an object";
                        return false;
                    }
                    for (const auto& [key, _] : m.items()) {
                        if (key != "side" && key != "adjacent_role") {
                            err = path + ": " + where + ".match." + key + ": unknown key";
                            return false;
                        }
                    }
                    if (!get_str(m, "side", rule.side, err, where + ".match") ||
                        !get_str(m, "adjacent_role", rule.adjacent_role, err, where + ".match"))
                        return false;
                    if (!rule.side.empty() && rule.side != "outer" && rule.side != "shared") {
                        err = path + ": " + where + ".match.side: expected outer|shared";
                        return false;
                    }
                    if (!rule.adjacent_role.empty() && !is_role_name(rule.adjacent_role)) {
                        err = path + ": " + where + ".match.adjacent_role: unknown role \"" +
                              rule.adjacent_role + "\"";
                        return false;
                    }
                }
                if (!r.contains("style") || !r["style"].is_string()) {
                    err = path + ": " + where + ".style: expected a style name";
                    return false;
                }
                rule.style = r["style"].get<std::string>();
                bool ok = false;
                style_code(rule.style, ok);
                if (!ok) {
                    err = path + ": " + where + ".style: unknown style \"" + rule.style + "\"";
                    return false;
                }
                fp.side_rules.push_back(std::move(rule));
            }
        }
    }
    if (doc.contains("slots")) {
        const auto& slots = doc["slots"];
        if (!slots.is_object()) {
            err = path + ": slots: expected an object";
            return false;
        }
        for (const auto& [kind, asset] : slots.items()) {
            if (!is_slot_kind(kind)) {
                err = path + ": slots." + kind + ": unknown slot kind";
                return false;
            }
            if (!asset.is_string() || asset.get<std::string>().empty()) {
                err = path + ": slots." + kind + ": expected a non-empty asset path";
                return false;
            }
            p.slots[kind] = asset.get<std::string>();
        }
    }
    if (doc.contains("asset_roots")) {
        const auto& roots = doc["asset_roots"];
        if (!roots.is_array()) {
            err = path + ": asset_roots: expected an array";
            return false;
        }
        for (const auto& r : roots) {
            if (!r.is_string()) {
                err = path + ": asset_roots: expected string entries";
                return false;
            }
            p.asset_roots.push_back(r.get<std::string>());
        }
    }
    if (p.asset_roots.empty()) p.asset_roots.push_back("assets");
    p.dir = fs::path(path).parent_path().string();

    // 5.4 invariants (the fill-tier subset checkable without layout tiers).
    const FillParams& fp = p.fill;
    if (!(fp.cell > 0.0)) {
        err = path + ": fill.cell must be > 0";
        return false;
    }
    if (!(fp.wall_t > 0.0 && fp.wall_t < fp.cell)) {
        err = path + ": fill.wall_t must satisfy 0 < wall_t (" + std::to_string(fp.wall_t) +
              ") < cell (" + std::to_string(fp.cell) + ") [5.4]";
        return false;
    }
    if (!(fp.frame >= 0.0)) {
        err = path + ": fill.frame must be >= 0";
        return false;
    }
    if (!(fp.row_module > 0.0)) {
        err = path + ": fill.row_module must be > 0";
        return false;
    }
    out = std::move(p);
    return true;
}

std::string room_role(bool corridor) { return corridor ? "corridor" : "hall"; }

RoleEntry resolve_role(const Project& project, const std::string& role) {
    RoleEntry out;  // hard defaults
    const auto& roles = project.fill.roles;
    if (const auto it = roles.find("*"); it != roles.end()) out = it->second;
    if (const auto it = roles.find(role); it != roles.end()) {
        // Named entries were already layered over "*" at load; take as-is.
        out = it->second;
    }
    // fill.room_h is the project-level default h when no roles are given at all.
    if (roles.empty()) out.h = project.fill.room_h;
    return out;
}

std::string resolve_side_style(const Project& project, const std::string& room_role, bool outer,
                               const std::string& adjacent_role) {
    std::string style = resolve_role(project, room_role).style;
    for (const auto& rule : project.fill.side_rules) {
        if (!rule.side.empty()) {
            const bool want_outer = rule.side == "outer";
            if (want_outer != outer) continue;
        }
        if (!rule.adjacent_role.empty()) {
            if (outer || rule.adjacent_role != adjacent_role) continue;
        }
        style = rule.style;  // later rules win
    }
    return style;
}

int style_code(const std::string& name, bool& ok) {
    ok = true;
    if (name == "stone") return 1;
    if (name == "brick") return 2;
    if (name == "plain") return 3;
    if (name == "mortar") return 4;
    ok = false;
    return 0;
}

int role_code(const std::string& name, bool& ok) {
    ok = true;
    if (name == "hall") return 1;
    if (name == "corridor") return 2;
    if (name == "crypt") return 3;
    if (name == "entry") return 4;
    if (name == "stairs") return 5;
    ok = false;
    return 0;
}

int pattern_code(const std::string& name, bool& ok) {
    ok = true;
    if (name == "butt") return 0;
    if (name == "chase") return 1;
    ok = false;
    return 0;
}

int door_code(const std::string& name, bool& ok) {
    ok = true;
    if (name == "open") return 1;
    if (name == "gate") return 2;
    ok = false;
    return 0;
}

int decor_code(const std::string& name, bool& ok) {
    ok = true;
    if (name == "lamp") return 1;
    ok = false;
    return 0;
}

}  // namespace delve
