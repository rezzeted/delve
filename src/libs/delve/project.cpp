#include "pch.h"

#include "project.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
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

// v1 cross-tier checks (R-G3, 5.4). Layout and fill are parsed.
static bool check_project_v1(const Project& p, const std::string& path, std::string& err) {
    const LayoutParams& l = *p.layout;
    const FillParams& f = p.fill;
    const double cell = f.cell;

    std::string bad;
    if (!check_connected(l, bad)) {
        err = path + ": layout.passages: room \"" + bad + "\" is unreachable (graph must be connected)";
        return false;
    }
    std::map<std::string, std::string> role_of;
    for (const auto& r : l.rooms) role_of[r.id] = r.role;
    for (const auto& r : l.rooms) {
        if (r.role != "corridor") continue;
        const auto nb = passage_neighbors(l, r.id);
        if (nb.size() != 2) {
            err = path + ": layout.rooms: corridor \"" + r.id + "\" has " +
                  std::to_string(nb.size()) + " neighbors, expected exactly 2";
            return false;
        }
        for (const auto& n : nb)
            if (role_of[n] == "corridor") {
                err = path + ": layout.passages: corridor \"" + r.id + "\" connects to corridor \"" +
                      n + "\" (corridors need non-corridor neighbors)";
                return false;
            }
    }
    if (const auto missing = roles_without_template(l); !missing.empty()) {
        err = path + ": layout: role \"" + missing.front() +
              "\" has no template (add an explicit template or widen rooms_rect.roles)";
        return false;
    }

    double wt_max = f.wall_t;
    for (const auto& [name, re] : f.roles)
        if (re.wall_t) {
            if (!(*re.wall_t < cell)) {
                err = path + ": fill.roles." + name + ".wall_t (" + std::to_string(*re.wall_t) +
                      ") must be < cell (" + std::to_string(cell) + ") [5.4]";
                return false;
            }
            wt_max = std::max(wt_max, *re.wall_t);
        }
    for (size_t i = 0; i < l.templates.size(); ++i) {
        if (!l.templates[i].fill.wall_t) continue;
        const double wt = *l.templates[i].fill.wall_t;
        if (!(wt < cell)) {
            err = path + ": layout.templates[" + std::to_string(i) + "].fill.wall_t (" +
                  std::to_string(wt) + ") must be < cell (" + std::to_string(cell) + ") [5.4]";
            return false;
        }
        wt_max = std::max(wt_max, wt);
    }
    for (size_t i = 0; i < l.rooms.size(); ++i) {
        if (!l.rooms[i].fill.wall_t) continue;
        const double wt = *l.rooms[i].fill.wall_t;
        if (!(wt < cell)) {
            err = path + ": layout.rooms[" + std::to_string(i) + "].fill.wall_t (" +
                  std::to_string(wt) + ") must be < cell (" + std::to_string(cell) + ") [5.4]";
            return false;
        }
        wt_max = std::max(wt_max, wt);
    }

    const double corr_clear = l.corridor_width * cell - wt_max;
    if (!(corr_clear >= f.min_passage)) {
        err = path + ": layout.corridors.width: clear width " + std::to_string(corr_clear) +
              " < min_passage " + std::to_string(f.min_passage) +
              " [5.4; raise width/cell or lower wall_t]";
        return false;
    }
    for (size_t i = 0; i < l.templates.size(); ++i) {
        const auto& t = l.templates[i];
        if (std::find(t.roles.begin(), t.roles.end(), "corridor") == t.roles.end()) continue;
        const double clear = template_min_bbox_side(t) * cell - wt_max;
        if (!(clear >= f.min_passage)) {
            err = path + ": layout.templates[" + std::to_string(i) +
                  "]: min bbox side gives clear width " + std::to_string(clear) + " < min_passage " +
                  std::to_string(f.min_passage) + " [5.4]";
            return false;
        }
    }

    const auto check_opening = [&](int len_cells, const std::string& where) {
        const double clear = len_cells * cell - 2.0 * f.frame;
        if (!(clear > 0.0) || !(clear >= f.min_opening)) {
            err = path + ": " + where + ": clear opening " + std::to_string(clear) +
                  " must be > 0 and >= min_opening " + std::to_string(f.min_opening) + " [5.4]";
            return false;
        }
        return true;
    };
    if (!check_opening(l.door_length, "layout.door_length")) return false;
    for (size_t i = 0; i < l.templates.size(); ++i) {
        const auto& t = l.templates[i];
        const std::string where = "layout.templates[" + std::to_string(i) + "]";
        if (!t.doors.manual && t.doors.length &&
            !check_opening(*t.doors.length, where + ".doors.length"))
            return false;
        if (t.doors.manual)
            for (size_t s = 0; s < t.doors.segments.size(); ++s) {
                const auto [a, b] = t.doors.segments[s];
                const int len = std::abs(a.first - b.first) + std::abs(a.second - b.second);
                if (!check_opening(len, where + ".doors.manual[" + std::to_string(s) + "]"))
                    return false;
            }
    }

    const auto check_corner = [&](int dist_cells, const std::string& where) {
        if (!(dist_cells * cell >= wt_max / 2.0 + f.frame)) {
            err = path + ": " + where + ": corner distance " + std::to_string(dist_cells) +
                  " cells < wall_t/2 + frame [5.4; the opening would reach the wall joint]";
            return false;
        }
        return true;
    };
    if (!check_corner(l.door_corner_distance, "layout.door_corner_distance")) return false;
    for (size_t i = 0; i < l.templates.size(); ++i) {
        const auto& t = l.templates[i];
        const std::string where = "layout.templates[" + std::to_string(i) + "]";
        if (!t.doors.manual && t.doors.corner_distance &&
            !check_corner(*t.doors.corner_distance, where + ".doors.corner_distance"))
            return false;
        if (t.doors.manual &&
            !check_corner(manual_door_min_corner(t), where + ".doors.manual (nearest corner)"))
            return false;
    }
    return true;
}

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
            key != "asset_roots" && key != "layout") {
            err = path + ": unknown key \"" + key + "\"";
            return false;
        }
    }
    const std::string format = doc.value("format", std::string{});
    const bool v1 = format == kProjectFormatV1;
    if (!v1 && format != kProjectFormat) {
        err = path + ": unsupported format \"" + format + "\", expected \"" + kProjectFormatV1 +
              "\" (or legacy \"" + kProjectFormat + "\")";
        return false;
    }
    p.format = format;
    if (!v1 && doc.contains("layout")) {
        err = path + ": layout: requires format \"" + std::string(kProjectFormatV1) + "\"";
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
                key != "lamp_place" && key != "row_module" && key != "roles" &&
                key != "transitions" && key != "side_rules" && key != "decor") {
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
            !get_str(f, "lamp_place", fp.lamp_place, err, "fill") ||
            !get_num(f, "row_module", fp.row_module, err, "fill"))
            return false;
        if (fp.lamp_place != "ceil" && fp.lamp_place != "wall") {
            err = path + ": fill.lamp_place: expected ceil|wall";
            return false;
        }
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
                    if (v1 && star.contains("wall_t") && star["wall_t"].is_number() &&
                        star["wall_t"].get<double>() > 0.0)
                        re.wall_t = star["wall_t"].get<double>();
                }
                for (const auto& [key, _] : entry.items()) {
                    const bool known = key == "h" || key == "style" || key == "floor" ||
                                       key == "ceil" || (v1 && key == "wall_t");
                    if (!known) {
                        err = path + ": fill.roles." + name + "." + key + ": unknown key";
                        return false;
                    }
                    re.set_fields.insert(key);  // F12: explicit on this level
                }
                const std::string where = "fill.roles." + name;
                if (!get_num(entry, "h", re.h, err, where) ||
                    !get_str(entry, "style", re.style, err, where) ||
                    !get_str(entry, "floor", re.floor, err, where) ||
                    !get_str(entry, "ceil", re.ceil, err, where))
                    return false;
                if (v1 && entry.contains("wall_t")) {
                    if (!entry["wall_t"].is_number() || !(entry["wall_t"].get<double>() > 0.0)) {
                        err = path + ": " + where + ".wall_t: expected a number > 0";
                        return false;
                    }
                    re.wall_t = entry["wall_t"].get<double>();
                }
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
        if (f.contains("decor")) {
            const auto& rules = f["decor"];
            if (!rules.is_array()) {
                err = path + ": fill.decor: expected an array";
                return false;
            }
            for (size_t i = 0; i < rules.size(); ++i) {
                const std::string where = "fill.decor[" + std::to_string(i) + "]";
                const auto& r = rules[i];
                if (!r.is_object()) {
                    err = path + ": " + where + ": expected an object";
                    return false;
                }
                for (const auto& [key, _] : r.items()) {
                    if (key != "tag" && key != "place" && key != "roles" && key != "chance" &&
                        key != "count" && key != "min_dist" && key != "align" &&
                        key != "radius" && key != "cut_r") {
                        err = path + ": " + where + "." + key + ": unknown key";
                        return false;
                    }
                }
                DecorRule rule;
                if (!r.contains("tag") || !r["tag"].is_string()) {
                    err = path + ": " + where + ".tag: expected a decor tag name";
                    return false;
                }
                rule.tag = r["tag"].get<std::string>();
                bool ok = false;
                decor_code(rule.tag, ok);
                if (!ok) {
                    err = path + ": " + where + ".tag: unknown decor tag \"" + rule.tag + "\"";
                    return false;
                }
                if (rule.tag == "lamp") {
                    err = path + ": " + where +
                          ".tag: \"lamp\" is placed via fill.lamp_step/lamp_place";
                    return false;
                }
                if (!get_str(r, "place", rule.place, err, where) ||
                    !get_num(r, "chance", rule.chance, err, where) ||
                    !get_num(r, "min_dist", rule.min_dist, err, where) ||
                    !get_str(r, "align", rule.align, err, where) ||
                    !get_num(r, "radius", rule.radius, err, where) ||
                    !get_num(r, "cut_r", rule.cut_r, err, where))
                    return false;
                if (rule.place != "floor" && rule.place != "wall") {
                    err = path + ": " + where + ".place: expected floor|wall";
                    return false;
                }
                if (rule.place == "wall") {
                    // align/cut_r steer floor pits only; on a wall rule they
                    // are dead keys — reject so a typo is not silently lost.
                    if (r.contains("align")) {
                        err = path + ": " + where + ".align: floor-only key on a wall rule";
                        return false;
                    }
                    if (r.contains("cut_r")) {
                        err = path + ": " + where + ".cut_r: floor-only key on a wall rule";
                        return false;
                    }
                }
                if (!(rule.chance >= 0.0 && rule.chance <= 1.0)) {
                    err = path + ": " + where + ".chance: expected 0..1";
                    return false;
                }
                if (r.contains("count")) {
                    if (!r["count"].is_number_integer() || r["count"].get<int>() < 1) {
                        err = path + ": " + where + ".count: expected an integer >= 1";
                        return false;
                    }
                    rule.count = r["count"].get<int>();
                }
                if (!(rule.min_dist >= 0.0)) {
                    err = path + ": " + where + ".min_dist: expected >= 0";
                    return false;
                }
                if (rule.align != "any" && rule.align != "center" && rule.align != "near_door") {
                    err = path + ": " + where + ".align: expected any|center|near_door";
                    return false;
                }
                if (!(rule.radius > 0.0)) {
                    err = path + ": " + where + ".radius: expected > 0";
                    return false;
                }
                if (!(rule.cut_r >= 0.0)) {
                    err = path + ": " + where + ".cut_r: expected >= 0";
                    return false;
                }
                if (r.contains("roles")) {
                    const auto& rls = r["roles"];
                    if (!rls.is_array()) {
                        err = path + ": " + where + ".roles: expected an array";
                        return false;
                    }
                    for (const auto& rn : rls) {
                        if (!rn.is_string() || rn.get<std::string>() == "*" ||
                            !is_role_name(rn.get<std::string>())) {
                            err = path + ": " + where +
                                  ".roles: unknown role (hall/corridor/crypt/entry/stairs)";
                            return false;
                        }
                        rule.roles.push_back(rn.get<std::string>());
                    }
                }
                fp.decor.push_back(std::move(rule));
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
    if (v1) {
        if (!doc.contains("layout")) {
            err = path + ": layout: required in \"" + std::string(kProjectFormatV1) + "\"";
            return false;
        }
        LayoutParams lp;
        if (!parse_layout(doc["layout"], path, lp, err)) return false;
        p.layout = std::move(lp);
        if (!check_project_v1(p, path, err)) return false;
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

namespace {

std::string fmt_val(double v) {
    std::ostringstream s;
    s << v;
    return s.str();
}

std::string field_val(const RoleEntry& e, const std::string& field) {
    if (field == "h") return fmt_val(e.h);
    if (field == "style") return e.style;
    if (field == "floor") return e.floor;
    if (field == "ceil") return e.ceil;
    return e.wall_t ? fmt_val(*e.wall_t) : std::string();  // wall_t
}

// F12: role-level steps of one field's chain (default/project -> "*" -> role;
// only levels where the field is explicit, set_fields tracked at load).
ProvChain role_field_chain(const Project& project, const std::string& role,
                           const std::string& field) {
    ProvChain chain;
    const auto& roles = project.fill.roles;
    if (roles.empty()) {
        if (field == "h")
            chain.push_back({"project", "", fmt_val(project.fill.room_h)});
        else if (field == "wall_t")
            chain.push_back({"project", "", fmt_val(project.fill.wall_t)});
        else
            chain.push_back({"default", "", field_val(RoleEntry{}, field)});
        return chain;
    }
    const auto star = roles.find("*");
    if (star != roles.end() && star->second.set_fields.count(field))
        chain.push_back({"role", "*", field_val(star->second, field)});
    if (role != "*") {
        const auto named = roles.find(role);
        if (named != roles.end() && named->second.set_fields.count(field))
            chain.push_back({"role", role, field_val(named->second, field)});
    }
    if (chain.empty()) {
        if (field == "wall_t")
            chain.push_back({"project", "", fmt_val(project.fill.wall_t)});
        else
            chain.push_back({"default", "", field_val(RoleEntry{}, field)});
    }
    return chain;
}

}  // namespace

RoleProvenance resolve_role_prov(const Project& project, const std::string& role) {
    RoleProvenance out;
    out.entry = resolve_role(project, role);
    for (const char* f : {"h", "style", "floor", "ceil", "wall_t"})
        out.prov[f] = role_field_chain(project, role, f);
    return out;
}

SideResolution apply_side_rules_prov(const Project& project, const std::string& base_style,
                                     bool outer, const std::string& adjacent_role) {
    SideResolution out;
    out.style = base_style;
    for (size_t i = 0; i < project.fill.side_rules.size(); ++i) {
        const SideRule& rule = project.fill.side_rules[i];
        if (!rule.side.empty()) {
            const bool want_outer = rule.side == "outer";
            if (want_outer != outer) continue;
        }
        if (!rule.adjacent_role.empty()) {
            if (outer || rule.adjacent_role != adjacent_role) continue;
        }
        out.style = rule.style;  // later rules win
        out.fired.push_back(static_cast<int>(i));
    }
    return out;
}

std::string apply_side_rules(const Project& project, const std::string& base_style, bool outer,
                             const std::string& adjacent_role) {
    return apply_side_rules_prov(project, base_style, outer, adjacent_role).style;
}

std::string resolve_side_style(const Project& project, const std::string& room_role, bool outer,
                               const std::string& adjacent_role) {
    return apply_side_rules(project, resolve_role(project, room_role).style, outer, adjacent_role);
}

std::string side_rule_detail(const Project& project, int index) {
    std::string keys;
    const SideRule& rule = project.fill.side_rules.at(static_cast<size_t>(index));
    if (!rule.side.empty()) keys += "side=" + rule.side;
    if (!rule.adjacent_role.empty()) {
        if (!keys.empty()) keys += ", ";
        keys += "adjacent_role=" + rule.adjacent_role;
    }
    return "side_rules[" + std::to_string(index) + "] (" + keys + ")";
}

std::string format_prov(const ProvChain& chain) {
    if (chain.empty()) return "<no provenance>";
    auto step_text = [](const ProvStep& s, bool winner) {
        std::string t = s.level;
        if (!s.detail.empty()) t += " \"" + s.detail + "\"";
        if (!winner) t += " (" + s.value + ")";
        return t;
    };
    std::string out = chain.back().value + " <- " + step_text(chain.back(), true);
    for (size_t i = chain.size() - 1; i-- > 0;) out += " <- " + step_text(chain[i], false);
    return out;
}

ResolvedFill resolve_room_fill(const Project& project, const std::string& role,
                               const FillOverride* tmpl, const FillOverride* room,
                               const std::string& tmpl_name, const std::string& room_id) {
    ResolvedFill out;
    const RoleProvenance base = resolve_role_prov(project, role);
    out.h = base.entry.h;
    out.style = base.entry.style;
    out.floor = base.entry.floor;
    out.ceil = base.entry.ceil;
    out.wall_t = base.entry.wall_t.value_or(project.fill.wall_t);
    out.prov = base.prov;
    auto apply = [&](const FillOverride* o, const char* level, const std::string& detail) {
        if (!o) return;
        if (o->h) {
            out.h = *o->h;
            out.prov["h"].push_back({level, detail, fmt_val(out.h)});
        }
        if (o->wall_t) {
            out.wall_t = *o->wall_t;
            out.prov["wall_t"].push_back({level, detail, fmt_val(out.wall_t)});
        }
        if (o->style) {
            out.style = *o->style;
            out.prov["style"].push_back({level, detail, out.style});
        }
        if (o->floor) {
            out.floor = *o->floor;
            out.prov["floor"].push_back({level, detail, out.floor});
        }
        if (o->ceil) {
            out.ceil = *o->ceil;
            out.prov["ceil"].push_back({level, detail, out.ceil});
        }
    };
    apply(tmpl, "template", tmpl_name);
    apply(room, "room", room_id);
    return out;
}

ResolvedFill resolve_room_fill(const Project& project, const std::string& role,
                               const FillOverride* tmpl, const FillOverride* room) {
    return resolve_room_fill(project, role, tmpl, room, "", "");
}

bool is_style_name(const std::string& name) {
    bool ok = false;
    style_code(name, ok);
    return ok;
}

bool is_door_name(const std::string& name) {
    bool ok = false;
    door_code(name, ok);
    return ok;
}

int style_code(const std::string& name, bool& ok) {
    ok = true;
    if (name == "stone") return 1;
    if (name == "brick") return 2;
    if (name == "plain") return 3;
    if (name == "mortar") return 4;
    if (name == "sandstone") return 5;
    if (name == "none") return 0;  // "no finish": room_fill may drop the element
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
    if (name == "drain") return 2;
    ok = false;
    return 0;
}

int anchor_code(const std::string& name, bool& ok) {
    ok = true;
    if (name == "light") return 1;
    if (name == "spawn") return 2;
    if (name == "poi") return 3;
    if (name == "blocker") return 4;
    ok = false;
    return 0;
}

uint32_t fnv1a_32_str(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

int layout_seed(int seed) {
    return static_cast<int>(fnv1a_32_str("layout/" + std::to_string(seed)) & 0x7fffffff);
}

int fill_seed_v1(int seed) {
    return static_cast<int>(fnv1a_32_str("fill/" + std::to_string(seed)) & 0x7fffffff);
}

}  // namespace delve
