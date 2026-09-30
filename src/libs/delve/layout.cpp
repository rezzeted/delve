#include "pch.h"

#include "layout.h"

#include <algorithm>
#include <cstdlib>
#include <set>

namespace delve {
namespace {

bool is_transform_name(const std::string& name) {
    return name == "identity" || name == "rot90" || name == "rot180" || name == "rot270" ||
           name == "mirror_x" || name == "mirror_y" || name == "diag13" || name == "diag24";
}

bool get_int(const nlohmann::json& j, const std::string& key, int& out, std::string& err,
             const std::string& where, int lo, const char* what) {
    const auto it = j.find(key);
    if (it == j.end()) return true;  // absent = default
    if (!it->is_number_integer()) {
        err = where + "." + key + ": expected an integer " + what + " (cells)";
        return false;
    }
    out = it->get<int>();
    if (out < lo) {
        err = where + "." + key + ": expected >= " + std::to_string(lo) + ", got " +
              std::to_string(out);
        return false;
    }
    return true;
}

bool get_range(const nlohmann::json& j, const std::string& key, IntRange& out, std::string& err,
               const std::string& where) {
    const auto it = j.find(key);
    if (it == j.end()) return true;
    const std::string w = where + "." + key;
    if (!it->is_array() || it->size() != 2 || !(*it)[0].is_number_integer() ||
        !(*it)[1].is_number_integer()) {
        err = w + ": expected [min, max] integers (cells)";
        return false;
    }
    out.lo = (*it)[0].get<int>();
    out.hi = (*it)[1].get<int>();
    if (out.lo < 1 || out.hi < out.lo) {
        err = w + ": expected 1 <= min <= max, got [" + std::to_string(out.lo) + ", " +
              std::to_string(out.hi) + "]";
        return false;
    }
    return true;
}

bool get_cell_pt(const nlohmann::json& j, CellPt& out, std::string& err, const std::string& where) {
    if (!j.is_array() || j.size() != 2 || !j[0].is_number_integer() || !j[1].is_number_integer()) {
        err = where + ": expected [x, y] integers (cells)";
        return false;
    }
    out = {j[0].get<int>(), j[1].get<int>()};
    return true;
}

}  // namespace

long long contour_area2(const std::vector<CellPt>& c) {
    long long a = 0;
    for (size_t i = 0; i < c.size(); ++i) {
        const auto [x0, y0] = c[i];
        const auto [x1, y1] = c[(i + 1) % c.size()];
        a += static_cast<long long>(x0) * y1 - static_cast<long long>(x1) * y0;
    }
    return a;
}

namespace {

// Closed intersection of two axis-aligned segments (touch counts).
bool ortho_segs_touch(CellPt a, CellPt b, CellPt c, CellPt d) {
    const bool ab_vert = a.first == b.first;
    const bool cd_vert = c.first == d.first;
    if (ab_vert && cd_vert) {
        if (a.first != c.first) return false;
        const int lo = std::max(std::min(a.second, b.second), std::min(c.second, d.second));
        const int hi = std::min(std::max(a.second, b.second), std::max(c.second, d.second));
        return lo <= hi;
    }
    if (!ab_vert && !cd_vert) {
        if (a.second != c.second) return false;
        const int lo = std::max(std::min(a.first, b.first), std::min(c.first, d.first));
        const int hi = std::min(std::max(a.first, b.first), std::max(c.first, d.first));
        return lo <= hi;
    }
    const CellPt v = ab_vert ? a : c, v2 = ab_vert ? b : d;
    const CellPt h = ab_vert ? c : a, h2 = ab_vert ? d : b;
    const int x = v.first, y = h.second;
    return x >= std::min(h.first, h2.first) && x <= std::max(h.first, h2.first) &&
           y >= std::min(v.second, v2.second) && y <= std::max(v.second, v2.second);
}

bool point_on_seg(CellPt p, CellPt a, CellPt b) {
    if (a.first == b.first)
        return p.first == a.first && p.second >= std::min(a.second, b.second) &&
               p.second <= std::max(a.second, b.second);
    return p.second == a.second && p.first >= std::min(a.first, b.first) &&
           p.first <= std::max(a.first, b.first);
}

bool parse_contour(const nlohmann::json& j, const std::string& path, const std::string& where,
                   std::vector<CellPt>& out, std::string& err) {
    if (!j.is_array() || j.size() < 4) {
        err = path + ": " + where + ": expected an array of >= 4 [x, y] points";
        return false;
    }
    std::vector<CellPt> c;
    for (size_t i = 0; i < j.size(); ++i) {
        CellPt p;
        if (!get_cell_pt(j[i], p, err, path + ": " + where + "[" + std::to_string(i) + "]"))
            return false;
        c.push_back(p);
    }
    const size_t n = c.size();
    for (size_t i = 0; i < n; ++i) {
        const CellPt a = c[i], b = c[(i + 1) % n];
        if (a == b) {
            err = path + ": " + where + ": zero-length edge at point " + std::to_string(i);
            return false;
        }
        if (a.first != b.first && a.second != b.second) {
            err = path + ": " + where + ": edge " + std::to_string(i) + " is not axis-aligned";
            return false;
        }
    }
    if (std::set<CellPt>(c.begin(), c.end()).size() != n) {
        err = path + ": " + where + ": duplicate points";
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        // edgar's PolygonGrid2D rejects these at construction; report at F1.
        const CellPt a = c[i], b = c[(i + 1) % n], d = c[(i + 2) % n];
        if ((a.first == b.first && b.first == d.first) ||
            (a.second == b.second && b.second == d.second)) {
            err = path + ": " + where + ": redundant vertex at point " +
                  std::to_string((i + 1) % n) + " (three collinear consecutive points)";
            return false;
        }
    }
    if (contour_area2(c) == 0) {
        err = path + ": " + where + ": zero area";
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = i + 1; j < n; ++j) {
            if (j == i + 1 || (i == 0 && j == n - 1)) continue;  // adjacent edges
            if (ortho_segs_touch(c[i], c[(i + 1) % n], c[j], c[(j + 1) % n])) {
                err = path + ": " + where + ": self-intersection at edges " + std::to_string(i) +
                      " and " + std::to_string(j);
                return false;
            }
        }
    }
    out = std::move(c);
    return true;
}

bool parse_template_doors(const nlohmann::json& j, const std::string& path, const std::string& where,
                          const std::vector<CellPt>& contour, TemplateDoors& out, std::string& err) {
    if (j.is_string()) {
        if (j.get<std::string>() != "simple") {
            err = path + ": " + where + ": expected \"simple\" or an object";
            return false;
        }
        return true;
    }
    if (!j.is_object()) {
        err = path + ": " + where + ": expected \"simple\" or an object";
        return false;
    }
    for (const auto& [key, _] : j.items()) {
        if (key != "length" && key != "corner_distance" && key != "manual") {
            err = path + ": " + where + "." + key + ": unknown key";
            return false;
        }
    }
    const bool has_manual = j.contains("manual");
    if (has_manual && (j.contains("length") || j.contains("corner_distance"))) {
        err = path + ": " + where + ": manual doors exclude length/corner_distance";
        return false;
    }
    if (!has_manual) {
        int length = 1, corner = 0;
        // Presence-tracked: read raw to distinguish absent (project default).
        if (j.contains("length")) {
            if (!get_int(j, "length", length, err, path + ": " + where, 1, "door length")) return false;
            out.length = length;
        }
        if (j.contains("corner_distance")) {
            if (!get_int(j, "corner_distance", corner, err, path + ": " + where, 0,
                         "corner distance"))
                return false;
            out.corner_distance = corner;
        }
        return true;
    }
    const auto& m = j["manual"];
    if (!m.is_array() || m.empty()) {
        err = path + ": " + where + ".manual: expected a non-empty array of segments";
        return false;
    }
    out.manual = true;
    for (size_t i = 0; i < m.size(); ++i) {
        const std::string w = where + ".manual[" + std::to_string(i) + "]";
        if (!m[i].is_array() || m[i].size() != 2) {
            err = path + ": " + w + ": expected [[x0, y0], [x1, y1]]";
            return false;
        }
        CellPt a, b;
        if (!get_cell_pt(m[i][0], a, err, path + ": " + w + "[0]")) return false;
        if (!get_cell_pt(m[i][1], b, err, path + ": " + w + "[1]")) return false;
        if (a == b || (a.first != b.first && a.second != b.second)) {
            err = path + ": " + w + ": expected a non-zero axis-aligned segment";
            return false;
        }
        bool a_on = false, b_on = false;
        for (size_t e = 0; e < contour.size(); ++e) {
            const CellPt c0 = contour[e], c1 = contour[(e + 1) % contour.size()];
            a_on = a_on || point_on_seg(a, c0, c1);
            b_on = b_on || point_on_seg(b, c0, c1);
        }
        if (!a_on || !b_on) {
            err = path + ": " + w + ": endpoints must lie on the template contour";
            return false;
        }
        out.segments.push_back({a, b});
    }
    return true;
}

}  // namespace

bool is_concrete_role(const std::string& name) {
    return name == "hall" || name == "corridor" || name == "crypt" || name == "entry" ||
           name == "stairs";
}

bool parse_layout(const nlohmann::json& j, const std::string& path, LayoutParams& out,
                  std::string& err) {
    if (!j.is_object()) {
        err = path + ": layout: expected an object";
        return false;
    }
    for (const auto& [key, _] : j.items()) {
        if (key != "corridors" && key != "rooms_rect" && key != "door_length" &&
            key != "door_corner_distance" && key != "min_room_distance" && key != "catalog_budget" &&
            key != "rooms" && key != "passages" && key != "templates") {
            err = path + ": layout." + key + ": unknown key";
            return false;
        }
    }
    LayoutParams l;
    if (j.contains("corridors")) {
        const auto& c = j["corridors"];
        if (!c.is_object()) {
            err = path + ": layout.corridors: expected an object";
            return false;
        }
        for (const auto& [key, _] : c.items()) {
            if (key != "width" && key != "length") {
                err = path + ": layout.corridors." + key + ": unknown key";
                return false;
            }
        }
        if (!get_int(c, "width", l.corridor_width, err, path + ": layout.corridors", 1,
                     "corridor width") ||
            !get_range(c, "length", l.corridor_length, err, path + ": layout.corridors"))
            return false;
    }
    if (j.contains("rooms_rect")) {
        const auto& r = j["rooms_rect"];
        if (!r.is_object()) {
            err = path + ": layout.rooms_rect: expected an object";
            return false;
        }
        for (const auto& [key, _] : r.items()) {
            if (key != "w" && key != "h" && key != "roles") {
                err = path + ": layout.rooms_rect." + key + ": unknown key";
                return false;
            }
        }
        if (!get_range(r, "w", l.rect_w, err, path + ": layout.rooms_rect") ||
            !get_range(r, "h", l.rect_h, err, path + ": layout.rooms_rect"))
            return false;
        if (r.contains("roles")) {
            const auto& roles = r["roles"];
            if (!roles.is_array() || roles.empty()) {
                err = path + ": layout.rooms_rect.roles: expected a non-empty array";
                return false;
            }
            l.rect_roles_set = true;
            std::set<std::string> seen;
            for (size_t i = 0; i < roles.size(); ++i) {
                if (!roles[i].is_string()) {
                    err = path + ": layout.rooms_rect.roles[" + std::to_string(i) +
                          "]: expected a role name";
                    return false;
                }
                const std::string name = roles[i].get<std::string>();
                if (!is_concrete_role(name) || name == "corridor") {
                    err = path + ": layout.rooms_rect.roles[" + std::to_string(i) +
                          "]: expected a non-corridor role";
                    return false;
                }
                if (seen.insert(name).second) l.rect_roles.push_back(name);
            }
        }
    }
    if (!get_int(j, "door_length", l.door_length, err, path + ": layout", 1, "door length") ||
        !get_int(j, "door_corner_distance", l.door_corner_distance, err, path + ": layout", 0,
                 "corner distance") ||
        !get_int(j, "min_room_distance", l.min_room_distance, err, path + ": layout", 0,
                 "minimum room distance") ||
        !get_int(j, "catalog_budget", l.catalog_budget, err, path + ": layout", 1, "budget"))
        return false;

    if (!j.contains("rooms") || !j["rooms"].is_array() || j["rooms"].empty()) {
        err = path + ": layout.rooms: expected a non-empty array";
        return false;
    }
    {
        std::set<std::string> ids;
        for (size_t i = 0; i < j["rooms"].size(); ++i) {
            const std::string where = "layout.rooms[" + std::to_string(i) + "]";
            const auto& r = j["rooms"][i];
            if (!r.is_object()) {
                err = path + ": " + where + ": expected an object";
                return false;
            }
            for (const auto& [key, _] : r.items()) {
                if (key != "id" && key != "role" && key != "tags" && key != "fill") {
                    err = path + ": " + where + "." + key + ": unknown key";
                    return false;
                }
            }
            GraphRoom room;
            if (!r.contains("id") || !r["id"].is_string() || r["id"].get<std::string>().empty()) {
                err = path + ": " + where + ".id: expected a non-empty string";
                return false;
            }
            room.id = r["id"].get<std::string>();
            if (!ids.insert(room.id).second) {
                err = path + ": " + where + ".id: duplicate room \"" + room.id + "\"";
                return false;
            }
            if (!r.contains("role") || !r["role"].is_string() ||
                !is_concrete_role(r["role"].get<std::string>())) {
                err = path + ": " + where + ".role: expected hall|corridor|crypt|entry|stairs";
                return false;
            }
            room.role = r["role"].get<std::string>();
            if (r.contains("tags")) {
                if (!r["tags"].is_array()) {
                    err = path + ": " + where + ".tags: expected an array of strings";
                    return false;
                }
                for (size_t t = 0; t < r["tags"].size(); ++t) {
                    if (!r["tags"][t].is_string() || r["tags"][t].get<std::string>().empty()) {
                        err = path + ": " + where + ".tags[" + std::to_string(t) +
                              "]: expected a non-empty string";
                        return false;
                    }
                    room.tags.push_back(r["tags"][t].get<std::string>());
                }
            }
            if (r.contains("fill")) {
                if (!parse_fill_override(r["fill"], path, where + ".fill", room.fill, err))
                    return false;
            }
            l.rooms.push_back(std::move(room));
        }
    }

    if (j.contains("passages")) {
        if (!j["passages"].is_array()) {
            err = path + ": layout.passages: expected an array";
            return false;
        }
        std::set<std::string> ids;
        for (const auto& r : l.rooms) ids.insert(r.id);
        std::set<std::pair<std::string, std::string>> pairs;
        for (size_t i = 0; i < j["passages"].size(); ++i) {
            const std::string where = "layout.passages[" + std::to_string(i) + "]";
            const auto& p = j["passages"][i];
            if (!p.is_object()) {
                err = path + ": " + where + ": expected an object";
                return false;
            }
            for (const auto& [key, _] : p.items()) {
                if (key != "a" && key != "b" && key != "door") {
                    err = path + ": " + where + "." + key + ": unknown key";
                    return false;
                }
            }
            Passage e;
            if (!p.contains("a") || !p["a"].is_string() || !ids.count(p["a"].get<std::string>())) {
                err = path + ": " + where + ".a: expected a room id from layout.rooms";
                return false;
            }
            if (!p.contains("b") || !p["b"].is_string() || !ids.count(p["b"].get<std::string>())) {
                err = path + ": " + where + ".b: expected a room id from layout.rooms";
                return false;
            }
            e.a = p["a"].get<std::string>();
            e.b = p["b"].get<std::string>();
            if (e.a == e.b) {
                err = path + ": " + where + ": a room cannot connect to itself";
                return false;
            }
            const auto key =
                std::make_pair(std::min(e.a, e.b), std::max(e.a, e.b));
            if (!pairs.insert(key).second) {
                err = path + ": " + where + ": duplicate passage " + key.first + " - " + key.second;
                return false;
            }
            if (p.contains("door")) {
                if (!p["door"].is_string()) {
                    err = path + ": " + where + ".door: expected a door type name";
                    return false;
                }
                e.door = p["door"].get<std::string>();
            }
            if (!is_door_name(e.door)) {
                err = path + ": " + where + ".door: unknown door type \"" + e.door +
                      "\" (open|gate)";
                return false;
            }
            l.passages.push_back(std::move(e));
        }
    }

    if (j.contains("templates")) {
        if (!j["templates"].is_array()) {
            err = path + ": layout.templates: expected an array";
            return false;
        }
        std::set<std::string> names;
        for (size_t i = 0; i < j["templates"].size(); ++i) {
            const std::string where = "layout.templates[" + std::to_string(i) + "]";
            const auto& t = j["templates"][i];
            if (!t.is_object()) {
                err = path + ": " + where + ": expected an object";
                return false;
            }
            for (const auto& [key, _] : t.items()) {
                if (key != "name" && key != "roles" && key != "contour" && key != "doors" &&
                    key != "transforms" && key != "fill") {
                    err = path + ": " + where + "." + key + ": unknown key";
                    return false;
                }
            }
            TemplateDecl d;
            if (!t.contains("name") || !t["name"].is_string() ||
                t["name"].get<std::string>().empty()) {
                err = path + ": " + where + ".name: expected a non-empty string";
                return false;
            }
            d.name = t["name"].get<std::string>();
            if (!names.insert(d.name).second) {
                err = path + ": " + where + ".name: duplicate template \"" + d.name + "\"";
                return false;
            }
            if (!t.contains("roles") || !t["roles"].is_array() || t["roles"].empty()) {
                err = path + ": " + where + ".roles: expected a non-empty array";
                return false;
            }
            {
                std::set<std::string> seen;
                for (size_t r = 0; r < t["roles"].size(); ++r) {
                    if (!t["roles"][r].is_string() ||
                        !is_concrete_role(t["roles"][r].get<std::string>())) {
                        err = path + ": " + where + ".roles[" + std::to_string(r) +
                              "]: expected hall|corridor|crypt|entry|stairs";
                        return false;
                    }
                    if (seen.insert(t["roles"][r].get<std::string>()).second)
                        d.roles.push_back(t["roles"][r].get<std::string>());
                }
            }
            if (!t.contains("contour")) {
                err = path + ": " + where + ".contour: expected an array of [x, y] points";
                return false;
            }
            if (!parse_contour(t["contour"], path, where + ".contour", d.contour, err)) return false;
            if (t.contains("doors")) {
                if (!parse_template_doors(t["doors"], path, where + ".doors", d.contour, d.doors,
                                          err))
                    return false;
            }
            if (t.contains("transforms")) {
                if (!t["transforms"].is_array()) {
                    err = path + ": " + where + ".transforms: expected an array";
                    return false;
                }
                d.transforms_set = true;
                std::set<std::string> seen;
                for (size_t k = 0; k < t["transforms"].size(); ++k) {
                    if (!t["transforms"][k].is_string() ||
                        !is_transform_name(t["transforms"][k].get<std::string>())) {
                        err = path + ": " + where + ".transforms[" + std::to_string(k) +
                              "]: expected identity|rot90|rot180|rot270|mirror_x|mirror_y|diag13|diag24";
                        return false;
                    }
                    if (seen.insert(t["transforms"][k].get<std::string>()).second)
                        d.transforms.push_back(t["transforms"][k].get<std::string>());
                }
            }
            if (t.contains("fill")) {
                if (!parse_fill_override(t["fill"], path, where + ".fill", d.fill, err))
                    return false;
            }
            l.templates.push_back(std::move(d));
        }
    }

    const int size = catalog_size(l);
    if (size > l.catalog_budget) {
        err = path + ": layout: catalog size " + std::to_string(size) + " exceeds budget " +
              std::to_string(l.catalog_budget) + " (" +
              std::to_string(parametric_corridor_count(l)) + " corridor + " +
              std::to_string(parametric_rect_count(l)) + " rect + " +
              std::to_string(l.templates.size()) + " explicit; raise catalog_budget)";
        return false;
    }
    out = std::move(l);
    return true;
}

bool parse_fill_override(const nlohmann::json& j, const std::string& path, const std::string& where,
                         FillOverride& out, std::string& err) {
    if (!j.is_object()) {
        err = path + ": " + where + ": expected an object";
        return false;
    }
    for (const auto& [key, _] : j.items()) {
        if (key != "h" && key != "style" && key != "floor" && key != "ceil" && key != "wall_t") {
            err = path + ": " + where + "." + key + ": unknown key";
            return false;
        }
    }
    FillOverride o;
    if (j.contains("h")) {
        if (!j["h"].is_number() || !(j["h"].get<double>() > 0.0)) {
            err = path + ": " + where + ".h: expected a number > 0";
            return false;
        }
        o.h = j["h"].get<double>();
    }
    if (j.contains("wall_t")) {
        if (!j["wall_t"].is_number() || !(j["wall_t"].get<double>() > 0.0)) {
            err = path + ": " + where + ".wall_t: expected a number > 0";
            return false;
        }
        o.wall_t = j["wall_t"].get<double>();
    }
    for (const std::string key : {"style", "floor", "ceil"}) {
        if (!j.contains(key)) continue;
        if (!j[key].is_string()) {
            err = path + ": " + where + "." + key + ": expected a style name";
            return false;
        }
        const std::string name = j[key].get<std::string>();
        if (!is_style_name(name)) {
            err = path + ": " + where + "." + key + ": unknown style \"" + name + "\"";
            return false;
        }
        if (key == "style") o.style = name;
        if (key == "floor") o.floor = name;
        if (key == "ceil") o.ceil = name;
    }
    out = std::move(o);
    return true;
}

std::vector<std::string> passage_neighbors(const LayoutParams& l, const std::string& id) {
    std::vector<std::string> out;
    for (const auto& p : l.passages) {
        if (p.a == id) out.push_back(p.b);
        if (p.b == id) out.push_back(p.a);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool check_connected(const LayoutParams& l, std::string& bad_room) {
    if (l.rooms.empty()) return true;
    std::set<std::string> seen{l.rooms.front().id};
    std::vector<std::string> stack{l.rooms.front().id};
    while (!stack.empty()) {
        const std::string cur = stack.back();
        stack.pop_back();
        for (const auto& n : passage_neighbors(l, cur))
            if (seen.insert(n).second) stack.push_back(n);
    }
    for (const auto& r : l.rooms)
        if (!seen.count(r.id)) {
            bad_room = r.id;
            return false;
        }
    return true;
}

int parametric_corridor_count(const LayoutParams& l) {
    const bool any =
        std::any_of(l.rooms.begin(), l.rooms.end(), [](const GraphRoom& r) { return r.role == "corridor"; });
    if (!any) return 0;
    return l.corridor_length.hi - l.corridor_length.lo + 1;
}

int parametric_rect_count(const LayoutParams& l) {
    if (l.rect_roles_set) {
        const bool covered = std::any_of(l.rooms.begin(), l.rooms.end(), [&](const GraphRoom& r) {
            return r.role != "corridor" &&
                   std::find(l.rect_roles.begin(), l.rect_roles.end(), r.role) != l.rect_roles.end();
        });
        if (!covered) return 0;
    }
    return (l.rect_w.hi - l.rect_w.lo + 1) * (l.rect_h.hi - l.rect_h.lo + 1);
}

int catalog_size(const LayoutParams& l) {
    return parametric_corridor_count(l) + parametric_rect_count(l) +
           static_cast<int>(l.templates.size());
}

std::vector<std::string> roles_without_template(const LayoutParams& l) {
    std::set<std::string> roles;
    for (const auto& r : l.rooms) roles.insert(r.role);
    std::vector<std::string> out;
    for (const auto& role : roles) {
        bool covered = false;
        if (role == "corridor") {
            covered = parametric_corridor_count(l) > 0;
        } else if (!l.rect_roles_set) {
            covered = true;  // parametric rects cover every non-corridor role
        } else {
            covered = std::find(l.rect_roles.begin(), l.rect_roles.end(), role) != l.rect_roles.end();
        }
        if (!covered) {
            for (const auto& t : l.templates)
                if (std::find(t.roles.begin(), t.roles.end(), role) != t.roles.end()) {
                    covered = true;
                    break;
                }
        }
        if (!covered) out.push_back(role);
    }
    return out;
}

int template_min_bbox_side(const TemplateDecl& t) {
    int min_x = t.contour[0].first, max_x = min_x;
    int min_y = t.contour[0].second, max_y = min_y;
    for (const auto& [x, y] : t.contour) {
        min_x = std::min(min_x, x);
        max_x = std::max(max_x, x);
        min_y = std::min(min_y, y);
        max_y = std::max(max_y, y);
    }
    return std::min(max_x - min_x, max_y - min_y);
}

bool read_layout_json(const std::string& text, LayoutData& out, std::string& err) {
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(text);
    } catch (const std::exception& e) {
        err = std::string("layout: invalid JSON: ") + e.what();
        return false;
    }
    if (!doc.is_object()) {
        err = "layout: expected a JSON object";
        return false;
    }
    if (doc.value("format", std::string{}) != kLayoutFormat) {
        err = "layout: unsupported format \"" + doc.value("format", std::string{}) +
              "\", expected \"" + kLayoutFormat + "\" (regenerate the layout from the project)";
        return false;
    }
    LayoutData data;
    if (doc.contains("source")) {
        const auto& s = doc["source"];
        if (!s.is_object() || !s.contains("project") || !s["project"].is_string() ||
            !s.contains("seed") || !s["seed"].is_number_integer()) {
            err = "layout.source: expected {project: string, seed: int}";
            return false;
        }
        data.source_project = s["project"].get<std::string>();
        data.source_seed = s["seed"].get<int>();
    }
    if (!doc.contains("rooms") || !doc["rooms"].is_array()) {
        err = "layout.rooms: expected an array";
        return false;
    }
    std::set<std::string> ids;
    for (size_t i = 0; i < doc["rooms"].size(); ++i) {
        const std::string where = "layout.rooms[" + std::to_string(i) + "]";
        const auto& jr = doc["rooms"][i];
        if (!jr.is_object()) {
            err = where + ": expected an object";
            return false;
        }
        for (const auto& [key, _] : jr.items()) {
            if (key != "id" && key != "role" && key != "template" && key != "corridor" &&
                key != "grid" && key != "doors") {
                err = where + "." + key + ": unknown key";
                return false;
            }
        }
        LayoutRoomData r;
        if (!jr.contains("id") || !jr["id"].is_string() || jr["id"].get<std::string>().empty()) {
            err = where + ".id: expected a non-empty string";
            return false;
        }
        r.id = jr["id"].get<std::string>();
        if (!ids.insert(r.id).second) {
            err = where + ".id: duplicate room \"" + r.id + "\"";
            return false;
        }
        if (!jr.contains("role") || !jr["role"].is_string() ||
            !is_concrete_role(jr["role"].get<std::string>())) {
            err = where + ".role: expected hall|corridor|crypt|entry|stairs";
            return false;
        }
        r.role = jr["role"].get<std::string>();
        if (!jr.contains("template") || !jr["template"].is_string() ||
            jr["template"].get<std::string>().empty()) {
            err = where + ".template: expected a non-empty string";
            return false;
        }
        r.tmpl = jr["template"].get<std::string>();
        if (!jr.contains("corridor") || !jr["corridor"].is_boolean()) {
            err = where + ".corridor: expected a boolean";
            return false;
        }
        r.corridor = jr["corridor"].get<bool>();
        if (!jr.contains("grid") || !jr["grid"].is_array() || jr["grid"].size() < 4) {
            err = where + ".grid: expected an array of >= 4 [x, y] points";
            return false;
        }
        for (size_t k = 0; k < jr["grid"].size(); ++k) {
            CellPt p;
            if (!get_cell_pt(jr["grid"][k], p, err, where + ".grid[" + std::to_string(k) + "]"))
                return false;
            r.grid.push_back(p);
        }
        if (!jr.contains("doors") || !jr["doors"].is_array()) {
            err = where + ".doors: expected an array";
            return false;
        }
        for (size_t k = 0; k < jr["doors"].size(); ++k) {
            const std::string dw = where + ".doors[" + std::to_string(k) + "]";
            const auto& jd = jr["doors"][k];
            if (!jd.is_object() || !jd.contains("to") || !jd["to"].is_string() ||
                jd["to"].get<std::string>().empty()) {
                err = dw + ": expected {to: room id, grid: [p, p]}";
                return false;
            }
            LayoutRoomData::Door d;
            d.to = jd["to"].get<std::string>();
            if (!jd.contains("grid") || !jd["grid"].is_array() || jd["grid"].size() != 2) {
                err = dw + ": expected {to: room id, grid: [p, p]}";
                return false;
            }
            if (!get_cell_pt(jd["grid"][0], d.g0, err, dw + ".grid[0]")) return false;
            if (!get_cell_pt(jd["grid"][1], d.g1, err, dw + ".grid[1]")) return false;
            if (d.g1 < d.g0) std::swap(d.g0, d.g1);
            r.doors.push_back(std::move(d));
        }
        std::sort(r.doors.begin(), r.doors.end(),
                  [](const LayoutRoomData::Door& a, const LayoutRoomData::Door& b) {
                      if (a.to != b.to) return a.to < b.to;
                      if (a.g0 != b.g0) return a.g0 < b.g0;
                      return a.g1 < b.g1;
                  });
        data.rooms.push_back(std::move(r));
    }
    std::sort(data.rooms.begin(), data.rooms.end(),
              [](const LayoutRoomData& a, const LayoutRoomData& b) { return a.id < b.id; });
    out = std::move(data);
    return true;
}

int manual_door_min_corner(const TemplateDecl& t) {
    if (!t.doors.manual) return -1;
    int best = -1;
    const size_t n = t.contour.size();
    for (const auto& [a, b] : t.doors.segments) {
        for (const CellPt p : {a, b}) {
            for (size_t e = 0; e < n; ++e) {
                const CellPt c0 = t.contour[e], c1 = t.contour[(e + 1) % n];
                if (!point_on_seg(p, c0, c1)) continue;
                const int d = std::min(std::abs(p.first - c0.first) + std::abs(p.second - c0.second),
                                       std::abs(p.first - c1.first) + std::abs(p.second - c1.second));
                best = best < 0 ? d : std::min(best, d);
            }
        }
    }
    return best;
}

}  // namespace delve
