#include "pch.h"

#include "ir.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <sstream>
#include <tuple>

#include <nlohmann/json.hpp>

namespace delve {
namespace {

constexpr double kEps = 1e-9;

long long area2(const std::vector<GridPt>& c) {
    long long a = 0;
    for (size_t i = 0; i < c.size(); ++i) {
        const auto [x0, y0] = c[i];
        const auto [x1, y1] = c[(i + 1) % c.size()];
        a += static_cast<long long>(x0) * y1 - static_cast<long long>(x1) * y0;
    }
    return a;
}

uint32_t fnv1a_32(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

std::string fmt_pt(GridPt p) { return "(" + std::to_string(p.first) + "," + std::to_string(p.second) + ")"; }

std::string fmt_num(double v) {
    std::ostringstream s;
    s << v;
    return s.str();
}

GridPt lex_min(GridPt a, GridPt b) { return b < a ? b : a; }
GridPt lex_max(GridPt a, GridPt b) { return b < a ? a : b; }

GridPt unit_dir(GridPt d) {
    return {(d.first > 0) - (d.first < 0), (d.second > 0) - (d.second < 0)};
}

// Even-odd containment. Probes used here sit at half-integer coordinates and
// never land on an edge: an edge crossing the probe would be an atom closing
// the very direction being probed (all edges have integer coordinates).
bool point_in_poly(const std::vector<GridPt>& c, double x, double y) {
    bool inside = false;
    const size_t n = c.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const double xi = c[i].first, yi = c[i].second;
        const double xj = c[j].first, yj = c[j].second;
        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) inside = !inside;
    }
    return inside;
}

// --- frozen IR input (delve-ir/0): rects, 1-cell doors, paired --------------

struct FrozenDoor {
    int to = -1;
    GridPt d0, d1;  // lex-min first
};

struct FrozenRoom {
    int id = 0;
    bool corridor = false;
    std::vector<GridPt> grid;  // normalized: area2 < 0
    std::vector<FrozenDoor> doors;
};

bool get_grid_pt(const nlohmann::json& j, GridPt& out) {
    if (!j.is_array() || j.size() != 2 || !j[0].is_number_integer() || !j[1].is_number_integer())
        return false;
    out = {j[0].get<int>(), j[1].get<int>()};
    return true;
}

bool parse_frozen(const std::string& text, const std::string& path, std::vector<FrozenRoom>& rooms,
                  std::string& err) {
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(text);
    } catch (const std::exception& e) {
        err = path + ": invalid JSON: " + e.what();
        return false;
    }
    if (doc.value("format", std::string{}) != "delve-ir/0") {
        err = path + ": unsupported frozen IR format \"" + doc.value("format", std::string{}) +
              "\" (expected delve-ir/0)";
        return false;
    }
    if (!doc.contains("rooms") || !doc["rooms"].is_array()) {
        err = path + ": rooms: expected an array";
        return false;
    }
    std::set<int> ids;
    for (const auto& jr : doc["rooms"]) {
        FrozenRoom r;
        if (!jr.contains("id") || !jr["id"].is_number_integer()) {
            err = path + ": room: id: expected an integer";
            return false;
        }
        r.id = jr["id"].get<int>();
        if (!ids.insert(r.id).second) {
            err = path + ": duplicate room id " + std::to_string(r.id);
            return false;
        }
        if (!jr.contains("corridor") || !jr["corridor"].is_boolean()) {
            err = path + ": room " + std::to_string(r.id) + ": corridor: expected a bool";
            return false;
        }
        r.corridor = jr["corridor"].get<bool>();
        const std::string where = "room " + std::to_string(r.id);
        if (!jr.contains("grid") || !jr["grid"].is_array()) {
            err = path + ": " + where + ": grid: expected an array";
            return false;
        }
        for (const auto& jp : jr["grid"]) {
            GridPt p;
            if (!get_grid_pt(jp, p)) {
                err = path + ": " + where + ": grid: expected [gx, gy] integer pairs";
                return false;
            }
            r.grid.push_back(p);
        }
        if (r.grid.size() != 4) {
            err = path + ": " + where + ": figured rooms land on the layout path (D2.3b; got " +
                  std::to_string(r.grid.size()) + " vertices)";
            return false;
        }
        for (size_t i = 0; i < 4; ++i) {
            const GridPt p = r.grid[i], q = r.grid[(i + 1) % 4];
            if (p.first != q.first && p.second != q.second) {
                err = path + ": " + where + ": non-axis-aligned edge " + fmt_pt(p) + "-" + fmt_pt(q);
                return false;
            }
            if (p == q) {
                err = path + ": " + where + ": degenerate edge at " + fmt_pt(p);
                return false;
            }
        }
        if (area2(r.grid) == 0) {
            err = path + ": " + where + ": degenerate contour";
            return false;
        }
        if (area2(r.grid) > 0) std::reverse(r.grid.begin(), r.grid.end());  // §5.1: Delve normalizes
        if (jr.contains("doors")) {
            if (!jr["doors"].is_array()) {
                err = path + ": " + where + ": doors: expected an array";
                return false;
            }
            for (const auto& jd : jr["doors"]) {
                if (!jd.contains("to") || !jd["to"].is_number_integer() || !jd.contains("grid") ||
                    !jd["grid"].is_array() || jd["grid"].size() != 2) {
                    err = path + ": " + where + ": door: expected {to, grid: [p, q]}";
                    return false;
                }
                FrozenDoor d;
                d.to = jd["to"].get<int>();
                GridPt p, q;
                if (!get_grid_pt(jd["grid"][0], p) || !get_grid_pt(jd["grid"][1], q)) {
                    err = path + ": " + where + ": door: expected integer endpoints";
                    return false;
                }
                d.d0 = lex_min(p, q);
                d.d1 = lex_max(p, q);
                const int len = std::abs(d.d1.first - d.d0.first) + std::abs(d.d1.second - d.d0.second);
                if (d.d0.first != d.d1.first && d.d0.second != d.d1.second) {
                    err = path + ": " + where + ": door is not axis-aligned";
                    return false;
                }
                if (len != 1) {
                    err = path + ": " + where + ": multi-cell doors land on the layout path " +
                          "(D2.3b; got length " + std::to_string(len) + ")";
                    return false;
                }
                // On the room contour: colinear with a contour edge, within its span.
                bool on_edge = false;
                for (size_t i = 0; i < 4; ++i) {
                    const GridPt a = r.grid[i], b = r.grid[(i + 1) % 4];
                    if (a.first == b.first && d.d0.first == a.first) {
                        const int lo = std::min(a.second, b.second), hi = std::max(a.second, b.second);
                        if (lo <= d.d0.second && d.d1.second <= hi) on_edge = true;
                    }
                    if (a.second == b.second && d.d0.second == a.second) {
                        const int lo = std::min(a.first, b.first), hi = std::max(a.first, b.first);
                        if (lo <= d.d0.first && d.d1.first <= hi) on_edge = true;
                    }
                }
                if (!on_edge) {
                    err = path + ": " + where + ": door " + fmt_pt(d.d0) + "-" + fmt_pt(d.d1) +
                          " is not on the room contour";
                    return false;
                }
                r.doors.push_back(d);
            }
        }
        rooms.push_back(std::move(r));
    }
    std::sort(rooms.begin(), rooms.end(),
              [](const FrozenRoom& a, const FrozenRoom& b) { return a.id < b.id; });
    // Door pairing: A -> B must be listed back by B -> A with the same segment.
    std::map<int, const FrozenRoom*> by_id;
    for (const auto& r : rooms) by_id[r.id] = &r;
    for (const auto& r : rooms) {
        for (const auto& d : r.doors) {
            const auto it = by_id.find(d.to);
            if (it == by_id.end()) {
                err = path + ": room " + std::to_string(r.id) + ": door to unknown room " +
                      std::to_string(d.to);
                return false;
            }
            bool back = false;
            for (const auto& e : it->second->doors)
                if (e.to == r.id && e.d0 == d.d0 && e.d1 == d.d1) back = true;
            if (!back) {
                err = path + ": room " + std::to_string(r.id) + ": door " + fmt_pt(d.d0) + "-" +
                      fmt_pt(d.d1) + " to room " + std::to_string(d.to) +
                      " has no matching entry in room " + std::to_string(d.to);
                return false;
            }
        }
    }
    return true;
}

// --- general contour check (layout path; F1 validates project-side already) --

// Closed intersection of two axis-aligned segments (touch counts).
bool ortho_segs_touch(GridPt a, GridPt b, GridPt c, GridPt d) {
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
    const GridPt v = ab_vert ? a : c, v2 = ab_vert ? b : d;
    const GridPt h = ab_vert ? c : a, h2 = ab_vert ? d : b;
    const int x = v.first, y = h.second;
    return x >= std::min(h.first, h2.first) && x <= std::max(h.first, h2.first) &&
           y >= std::min(v.second, v2.second) && y <= std::max(v.second, v2.second);
}

bool check_contour(const std::vector<GridPt>& c, const std::string& where,
                   const std::string& path, std::string& err) {
    if (c.size() < 4) {
        err = path + ": " + where + ": expected >= 4 contour points";
        return false;
    }
    const size_t n = c.size();
    for (size_t i = 0; i < n; ++i) {
        const GridPt a = c[i], b = c[(i + 1) % n], d = c[(i + 2) % n];
        if (a == b) {
            err = path + ": " + where + ": zero-length edge at point " + std::to_string(i);
            return false;
        }
        if (a.first != b.first && a.second != b.second) {
            err = path + ": " + where + ": edge " + std::to_string(i) + " is not axis-aligned";
            return false;
        }
        if ((a.first == b.first && b.first == d.first) ||
            (a.second == b.second && b.second == d.second)) {
            err = path + ": " + where + ": redundant vertex at point " +
                  std::to_string((i + 1) % n) + " (three collinear consecutive points)";
            return false;
        }
    }
    if (std::set<GridPt>(c.begin(), c.end()).size() != n) {
        err = path + ": " + where + ": duplicate points";
        return false;
    }
    if (area2(c) == 0) {
        err = path + ": " + where + ": zero area";
        return false;
    }
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 1; j < n; ++j) {
            if (j == i + 1 || (i == 0 && j == n - 1)) continue;  // adjacent edges
            if (ortho_segs_touch(c[i], c[(i + 1) % n], c[j], c[(j + 1) % n])) {
                err = path + ": " + where + ": self-intersection at edges " + std::to_string(i) +
                      " and " + std::to_string(j);
                return false;
            }
        }
    return true;
}

// --- unified core input -----------------------------------------------------

struct DoorInput {
    std::string to;
    GridPt d0, d1;  // lex-min first
    int dtype = 1;  // DR_* code (frozen path: open)
};

struct RoomInput {
    std::string id;
    bool corridor = false;
    std::string role;
    std::vector<GridPt> grid;  // normalized (area2 < 0), any orthogonal simple polygon
    std::vector<DoorInput> doors;
    double h = 0, wall_t = 0;  // resolved fill (4.2)
    std::string style, floor_style, ceil_style;
    std::map<std::string, ProvChain> prov;  // F12: chains of the resolved values
};

// Contour edge of a room: axis line (vert, coord), span [t0, t1], t0 < t1.
struct Edge {
    std::string room;
    int index = -1;  // contour edge index (leaves vertex `index`)
    bool vert = false;
    int coord = 0;
    int t0 = 0, t1 = 0;
    bool room_on_neg = false;  // interior on the -x (vert) / -y (horiz) side
};

struct Atom {
    bool vert = false;
    int coord = 0;
    int t0 = 0, t1 = 0;
    std::string room_neg, room_pos;  // room on each side ("" = void)
};

struct LineKey {
    bool vert = false;
    int coord = 0;
    bool operator<(const LineKey& o) const {
        if (vert != o.vert) return vert < o.vert;
        return coord < o.coord;
    }
};

}  // namespace

int unit_seed(int fill_seed, const std::string& unit_id) {
    return static_cast<int>(fnv1a_32(std::to_string(fill_seed) + "/" + unit_id) & 0x7fffffff);
}

int zone_seed(int zone_id) {
    return static_cast<int>(fnv1a_32("transition/" + std::to_string(zone_id)) & 0x7fffffff);
}

namespace {

bool strictly_inside_edge(const RoomInput& fr, GridPt v) {
    for (size_t i = 0; i < fr.grid.size(); ++i) {
        const GridPt p = fr.grid[i], q = fr.grid[(i + 1) % fr.grid.size()];
        if (p.first == q.first && v.first == p.first &&
            std::min(p.second, q.second) < v.second && v.second < std::max(p.second, q.second))
            return true;
        if (p.second == q.second && v.second == p.second &&
            std::min(p.first, q.first) < v.first && v.first < std::max(p.first, q.first))
            return true;
    }
    return false;
}

// Shared F4 core (frozen and layout paths): atoms, walls, nodes, doors,
// developments, transitions, derived values. Inputs are sorted by room id.
bool build_core(const Project& project, const std::vector<RoomInput>& frooms,
                const std::string& err_path, IrV2& ir, std::string& err) {
    const double cell = project.fill.cell;

    std::map<std::string, size_t> room_idx;  // id -> frooms/ir.rooms index (same order)
    for (size_t i = 0; i < frooms.size(); ++i) room_idx[frooms[i].id] = i;
    auto room_of = [&](const std::string& id) -> const RoomInput& { return frooms[room_idx[id]]; };

    // --- 2. rooms ---
    for (const auto& fr : frooms) {
        IrRoom r;
        r.id = fr.id;
        r.corridor = fr.corridor;
        r.role = fr.role;
        r.grid = fr.grid;
        r.h = fr.h;
        r.style = fr.style;
        r.floor_style = fr.floor_style;
        r.ceil_style = fr.ceil_style;
        r.prov = fr.prov;
        ir.rooms.push_back(std::move(r));
    }

    // --- 3. atomize (§5.2 T-rule: every vertex on an edge splits it) ---
    // Interior side is taken from the walk direction (CW contour: interior on
    // the right), so any orthogonal simple polygon works, not just rects.
    std::map<LineKey, std::vector<Edge>> lines;
    for (const auto& fr : frooms) {
        for (size_t i = 0; i < fr.grid.size(); ++i) {
            const GridPt p = fr.grid[i], q = fr.grid[(i + 1) % fr.grid.size()];
            Edge e;
            e.room = fr.id;
            e.index = static_cast<int>(i);
            if (p.first == q.first) {
                e.vert = true;
                e.coord = p.first;
                e.t0 = std::min(p.second, q.second);
                e.t1 = std::max(p.second, q.second);
                // Walked down (-y): right of the walk is -x (neg); up: +x (pos).
                e.room_on_neg = q.second < p.second;
            } else {
                e.vert = false;
                e.coord = p.second;
                e.t0 = std::min(p.first, q.first);
                e.t1 = std::max(p.first, q.first);
                // Walked +x: right of the walk is -y (neg); -x: +y (pos).
                e.room_on_neg = q.first > p.first;
            }
            lines[{e.vert, e.coord}].push_back(e);
        }
    }
    std::vector<Atom> atoms;
    for (const auto& [key, edges] : lines) {
        std::set<int> splits;
        for (const auto& e : edges) {
            splits.insert(e.t0);
            splits.insert(e.t1);
        }
        std::vector<int> ts(splits.begin(), splits.end());
        for (size_t i = 0; i + 1 < ts.size(); ++i) {
            const int t0 = ts[i], t1 = ts[i + 1];
            std::set<std::string> neg, pos;
            for (const auto& e : edges) {
                if (!(e.t0 <= t0 && t1 <= e.t1)) continue;
                (e.room_on_neg ? neg : pos).insert(e.room);
            }
            if (neg.size() > 1 || pos.size() > 1) {
                err = err_path + ": rooms overlap on " + std::string(key.vert ? "x=" : "y=") +
                      std::to_string(key.coord) + " [" + std::to_string(t0) + "," +
                      std::to_string(t1) + "]";
                return false;
            }
            Atom a;
            a.vert = key.vert;
            a.coord = key.coord;
            a.t0 = t0;
            a.t1 = t1;
            a.room_neg = neg.empty() ? "" : *neg.begin();
            a.room_pos = pos.empty() ? "" : *pos.begin();
            if (a.room_neg.empty() && a.room_pos.empty()) continue;  // uncovered gap (cannot happen)
            atoms.push_back(a);
        }
    }

    auto atom_ends = [](const Atom& a) {
        GridPt p0, p1;
        if (a.vert) {
            p0 = {a.coord, a.t0};
            p1 = {a.coord, a.t1};
        } else {
            p0 = {a.t0, a.coord};
            p1 = {a.t1, a.coord};
        }
        return std::pair<GridPt, GridPt>(lex_min(p0, p1), lex_max(p0, p1));
    };

    // --- per-room atomized contour index (D3, F8: position-independent ids) --
    // For every room: its contour atoms per edge in walk order, the reverse
    // map atom -> (edge, k), and the walk index of every atomized-contour
    // vertex. Wall/node ids are derived from these, so a re-layout that moves
    // a room keeps the ids of its units (delve-ir/3).
    struct RoomContour {
        std::vector<std::vector<size_t>> edge_atoms;  // [contour edge] atom indices, walk order
        std::map<size_t, std::pair<size_t, size_t>> atom_pos;  // atom index -> (edge, k)
        std::map<GridPt, int> vertex_index;  // atomized-contour vertex -> walk index
    };
    std::map<std::string, RoomContour> contours;
    std::map<std::pair<GridPt, GridPt>, size_t> atom_by_ends;
    for (size_t ai = 0; ai < atoms.size(); ++ai) atom_by_ends[atom_ends(atoms[ai])] = ai;
    for (const auto& fr : frooms) {
        RoomContour rc;
        int walk = 0;
        for (size_t i = 0; i < fr.grid.size(); ++i) {
            const GridPt p = fr.grid[i], q = fr.grid[(i + 1) % fr.grid.size()];
            const bool vert = p.first == q.first;
            const int w = vert ? ((q.second > p.second) ? 1 : -1) : ((q.first > p.first) ? 1 : -1);
            std::vector<size_t> eas;
            for (size_t ai = 0; ai < atoms.size(); ++ai) {
                const Atom& a = atoms[ai];
                if (a.vert != vert || a.coord != (vert ? p.first : p.second)) continue;
                const int lo = vert ? std::min(p.second, q.second) : std::min(p.first, q.first);
                const int hi = vert ? std::max(p.second, q.second) : std::max(p.first, q.first);
                if (lo <= a.t0 && a.t1 <= hi) eas.push_back(ai);
            }
            std::sort(eas.begin(), eas.end(), [&](size_t x, size_t y) {
                const double mx = (atoms[x].t0 + atoms[x].t1) * 0.5;
                const double my = (atoms[y].t0 + atoms[y].t1) * 0.5;
                return w > 0 ? mx < my : mx > my;
            });
            rc.vertex_index.try_emplace(p, walk);
            for (size_t k = 0; k < eas.size(); ++k) {
                rc.atom_pos[eas[k]] = {i, k};
                ++walk;
                const Atom& a = atoms[eas[k]];
                const int e_t = (w > 0) ? a.t1 : a.t0;  // walk-end axis coord
                rc.vertex_index.try_emplace(vert ? GridPt{a.coord, e_t} : GridPt{e_t, a.coord},
                                            walk);
            }
            rc.edge_atoms.push_back(std::move(eas));
        }
        contours[fr.id] = std::move(rc);
    }
    auto wall_id_of = [&](size_t ai, const std::string& owner) {
        const RoomContour& rc = contours.at(owner);
        const auto [ei, k] = rc.atom_pos.at(ai);
        std::string id = "wall:" + owner + ":" + std::to_string(ei);
        if (rc.edge_atoms[ei].size() > 1) id += "." + std::to_string(k);
        return id;
    };
    auto node_id_of = [&](GridPt v, const std::string& owner) {
        return "node:" + owner + ":" + std::to_string(contours.at(owner).vertex_index.at(v));
    };
    std::vector<std::string> atom_wall_id(atoms.size());
    std::map<GridPt, std::string> node_id_at;

    // --- walls (§5.2: one body per atom, owner = min room id, thickness from
    // the owner; a shared wall whose sides resolve different thicknesses is an
    // F4 error naming both rooms) ---
    std::map<std::string, size_t> wall_idx;
    for (size_t ai = 0; ai < atoms.size(); ++ai) {
        const Atom& a = atoms[ai];
        const auto [g0, g1] = atom_ends(a);
        IrWall w;
        // Rooms left/right of the g0 -> g1 axis (grid math view, x right, y up).
        if (a.vert) {  // axis +y: left = -x = neg side
            w.room_left = a.room_neg;
            w.room_right = a.room_pos;
        } else {  // axis +x: left = +y = pos side
            w.room_left = a.room_pos;
            w.room_right = a.room_neg;
        }
        w.outer = w.room_left.empty() != w.room_right.empty();
        if (w.room_left.empty() && w.room_right.empty()) {
            err = err_path + ": internal: wall atom " + fmt_pt(g0) + "-" + fmt_pt(g1) +
                  " has no rooms";
            return false;
        }
        w.owner = w.outer ? (w.room_left.empty() ? w.room_right : w.room_left)
                          : std::min(w.room_left, w.room_right);
        w.id = wall_id_of(ai, w.owner);
        if (wall_idx.count(w.id)) {
            err = err_path + ": internal: duplicate wall " + w.id;
            return false;
        }
        if (!w.outer) {
            const double tl = room_of(w.room_left).wall_t, tr = room_of(w.room_right).wall_t;
            if (!(tl == tr)) {
                // F12: the error shows where each side's wall_t comes from.
                err = err_path + ": rooms " + w.room_left + " and " + w.room_right +
                      " resolve different wall thicknesses (" + fmt_num(tl) + " vs " +
                      fmt_num(tr) + ") for their shared wall " + w.id +
                      " [5.2; align wall_t on both roles/templates/rooms]" +
                      "\n  " + w.room_left +
                      " wall_t: " + format_prov(ir.rooms[room_idx[w.room_left]].prov["wall_t"]) +
                      "\n  " + w.room_right +
                      " wall_t: " + format_prov(ir.rooms[room_idx[w.room_right]].prov["wall_t"]);
                return false;
            }
        }
        w.g0 = g0;
        w.g1 = g1;
        w.thick = room_of(w.owner).wall_t;
        const double h_owner = ir.rooms[room_idx[w.owner]].h;
        w.h_left = w.room_left.empty() ? h_owner : ir.rooms[room_idx[w.room_left]].h;
        w.h_right = w.room_right.empty() ? h_owner : ir.rooms[room_idx[w.room_right]].h;
        atom_wall_id[ai] = w.id;
        wall_idx[w.id] = ir.walls.size();
        ir.walls.push_back(std::move(w));
    }

    // --- 5. nodes + open faces (§5.2; ahead of doors: the 5.4 offset check
    // needs pillar thicknesses) ---
    std::set<GridPt> vertices;
    for (const auto& a : atoms) {
        const auto [g0, g1] = atom_ends(a);
        vertices.insert(g0);
        vertices.insert(g1);
    }
    const GridPt kDirs[4] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    std::map<GridPt, size_t> node_idx;
    // (node id, face normal) -> face index, for zone-piece attachment.
    std::map<std::pair<std::string, GridPt>, size_t> face_idx;
    for (const GridPt v : vertices) {
        std::vector<const Atom*> incident;
        for (const auto& a : atoms) {
            const auto [g0, g1] = atom_ends(a);
            if (g0 == v || g1 == v) incident.push_back(&a);
        }
        if (incident.size() < 2) {
            err = err_path + ": internal: vertex " + fmt_pt(v) + " has " +
                  std::to_string(incident.size()) + " walls";
            return false;
        }
        std::set<std::string> adj;
        for (const auto* a : incident) {
            if (!a->room_neg.empty()) adj.insert(a->room_neg);
            if (!a->room_pos.empty()) adj.insert(a->room_pos);
        }
        IrNode node;
        node.id = node_id_of(v, *adj.begin());
        node.owner = *adj.begin();
        node.at = v;
        node.thick = room_of(node.owner).wall_t;
        node.h_pillar = 0;
        for (const auto& r : adj) node.h_pillar = std::max(node.h_pillar, ir.rooms[room_idx[r]].h);
        for (const GridPt n : kDirs) {
            bool closed = false;
            for (const auto* a : incident) {
                const auto [g0, g1] = atom_ends(*a);
                const GridPt o = (g0 == v) ? g1 : g0;
                const GridPt d = {o.first - v.first, o.second - v.second};
                if (d.first * n.second == d.second * n.first &&  // colinear
                    d.first * n.first + d.second * n.second > 0)
                    closed = true;
            }
            if (closed) continue;
            // Open face: the room it looks into, probed half a cell past the
            // face plane (works for figured rooms; on rect layouts this
            // provably coincides with the incident-room test).
            const double px = v.first + 0.5 * n.first, py = v.second + 0.5 * n.second;
            std::string beyond;
            for (const auto& fr : frooms) {
                if (!point_in_poly(fr.grid, px, py)) continue;
                if (!beyond.empty()) {
                    err = err_path + ": rooms overlap past vertex " + fmt_pt(v);
                    return false;
                }
                beyond = fr.id;
            }
            IrNodeFace f;
            f.center = {n.first * node.thick / 2.0, n.second * node.thick / 2.0};
            f.n = {static_cast<double>(n.first), static_cast<double>(n.second)};
            f.room = beyond;
            if (f.room.empty()) {
                f.h = ir.rooms[room_idx[node.owner]].h;
                f.style = ir.rooms[room_idx[node.owner]].style;
                f.prov["style"] = ir.rooms[room_idx[node.owner]].prov["style"];
            } else {
                f.h = ir.rooms[room_idx[f.room]].h;
                f.style = ir.rooms[room_idx[f.room]].style;  // joint pass refines T/concave faces
                f.prov["style"] = ir.rooms[room_idx[f.room]].prov["style"];
            }
            node.faces.push_back(std::move(f));
        }
        // Deterministic face order: lex by normal.
        std::sort(node.faces.begin(), node.faces.end(), [](const IrNodeFace& a, const IrNodeFace& b) {
            if (a.n.first != b.n.first) return a.n.first < b.n.first;
            return a.n.second < b.n.second;
        });
        node_idx[v] = ir.nodes.size();
        node_id_at[v] = node.id;
        ir.nodes.push_back(std::move(node));
    }
    for (size_t ni = 0; ni < ir.nodes.size(); ++ni) {
        IrNode& node = ir.nodes[ni];
        for (size_t fi = 0; fi < node.faces.size(); ++fi) {
            const GridPt n = {static_cast<int>(node.faces[fi].n.first),
                              static_cast<int>(node.faces[fi].n.second)};
            face_idx[{node.id, n}] = fi;
        }
    }

    // Wall bodies run between pillar faces: per-end pillar thickness (equal to
    // the wall's own under uniform wall_t; differs only at point contacts,
    // which the 5.2 working rule tolerates).
    for (auto& w : ir.walls) {
        w.t_end0 = ir.nodes[node_idx[w.g0]].thick;
        w.t_end1 = ir.nodes[node_idx[w.g1]].thick;
    }

    // --- 4. doors -> walls + 5.4 structural checks ---
    std::set<std::tuple<std::string, std::string, GridPt, GridPt>> door_pairs;
    for (const auto& fr : frooms)
        for (const auto& d : fr.doors)
            door_pairs.insert(
                {std::min(fr.id, d.to), std::max(fr.id, d.to), d.d0, d.d1});
    for (const auto& [ra, rb, dd0, dd1] : door_pairs) {
        const bool vert = dd0.first == dd1.first;
        const int coord = vert ? dd0.first : dd0.second;
        const int td0 = vert ? dd0.second : dd0.first;
        const int td1 = vert ? dd1.second : dd1.first;
        // Crossing check: a split strictly inside the door span.
        const auto lit = lines.find({vert, coord});
        if (lit == lines.end()) {
            err = err_path + ": internal: door of rooms " + ra + "-" + rb + " is on no line";
            return false;
        }
        for (const auto& e : lit->second) {
            if ((td0 < e.t0 && e.t0 < td1) || (td0 < e.t1 && e.t1 < td1)) {
                err = err_path + ": door " + fmt_pt(dd0) + "-" + fmt_pt(dd1) + " of rooms " + ra +
                      "-" + rb + " crosses a T-junction (vertices split walls, doors cannot span them)";
                return false;
            }
        }
        // Owning atom: the unique atom covering the door span.
        const Atom* owner_atom = nullptr;
        for (const auto& a : atoms) {
            if (a.vert != vert || a.coord != coord) continue;
            if (a.t0 <= td0 && td1 <= a.t1) owner_atom = &a;
        }
        if (!owner_atom) {
            err = err_path + ": door " + fmt_pt(dd0) + "-" + fmt_pt(dd1) + " of rooms " + ra +
                  "-" + rb + " lies on no wall";
            return false;
        }
        const auto [g0, g1] = atom_ends(*owner_atom);
        const std::string& wid = atom_wall_id[atom_by_ends.at({g0, g1})];
        IrWall& wall = ir.walls[wall_idx[wid]];
        const std::string wa = std::min(wall.room_left, wall.room_right);
        const std::string wb = std::max(wall.room_left, wall.room_right);
        if (wall.outer || wa != ra || wb != rb) {
            err = err_path + ": door " + fmt_pt(dd0) + "-" + fmt_pt(dd1) + " of rooms " + ra +
                  "-" + rb + " is not on their shared wall (" + wid + ")";
            return false;
        }
        int dtype = 1;
        for (const auto& fr : frooms)
            for (const auto& d : fr.doors)
                if (std::min(fr.id, d.to) == ra && std::max(fr.id, d.to) == rb && d.d0 == dd0 &&
                    d.d1 == dd1) {
                    dtype = d.dtype;
                    break;
                }
        IrDoor door;
        door.id = "door:" + ra + "-" + rb;
        door.room_a = ra;
        door.room_b = rb;
        door.wall = wid;
        door.g0 = dd0;
        door.g1 = dd1;
        door.h = project.fill.door_h;
        door.frame = project.fill.frame;
        door.thick = wall.thick;
        door.dtype = dtype;
        // F12: dtype from the passage edge (layout) / fixed open (frozen);
        // h/frame are project fill values; thick is the owner's wall_t.
        if (ir.from_layout)
            door.prov["dtype"] = ProvChain{{"passage", ra + "-" + rb, std::to_string(dtype)}};
        else
            door.prov["dtype"] = ProvChain{{"default", "", std::to_string(dtype)}};
        door.prov["h"] = ProvChain{{"project", "", fmt_num(door.h)}};
        door.prov["frame"] = ProvChain{{"project", "", fmt_num(door.frame)}};
        door.prov["thick"] = ir.rooms[room_idx[wall.owner]].prov["wall_t"];
        const int len_cells = std::abs(dd1.first - dd0.first) + std::abs(dd1.second - dd0.second);
        door.clear = len_cells * cell - 2.0 * door.frame;  // §5.3
        if (!(door.clear > 0.0)) {
            err = err_path + ": door " + door.id + ": clear opening " + fmt_num(door.clear) +
                  "m <= 0 (door_len * cell - 2 * frame; widen cell or narrow frame)";
            return false;
        }
        // 5.4: door offset from the corner (cells * cell >= pillar/2 + frame;
        // the pillar at an atom end may be thicker than the wall itself).
        const double t0n = ir.nodes[node_idx[g0]].thick, t1n = ir.nodes[node_idx[g1]].thick;
        const double pillar = std::max({wall.thick, t0n, t1n});
        const double off_cells = std::min(td0 - owner_atom->t0, owner_atom->t1 - td1);
        if (off_cells * cell < pillar / 2.0 + door.frame - kEps) {
            err = err_path + ": door " + door.id + ": offset from the corner " +
                  fmt_num(off_cells * cell) + "m < thick/2 + frame (" + fmt_num(pillar / 2.0) +
                  " + " + fmt_num(door.frame) + ") [5.4]";
            return false;
        }
        // Clear ends: full segment inset by frame (meters), lex-min first.
        const double ux = vert ? 0.0 : 1.0, uy = vert ? 1.0 : 0.0;
        door.from = {dd0.first * cell + ux * door.frame, dd0.second * cell + uy * door.frame};
        door.to = {dd1.first * cell - ux * door.frame, dd1.second * cell - uy * door.frame};
        wall.doors.push_back(door.id);
        ir.doors.push_back(std::move(door));
    }
    for (auto& w : ir.walls) std::sort(w.doors.begin(), w.doors.end());

    auto door_by_id = [&](const std::string& id) -> const IrDoor& {
        for (const auto& d : ir.doors)
            if (d.id == id) return d;
        static IrDoor empty;
        return empty;  // unreachable (ids come from walls)
    };

    // --- 6+7. developments + transitions ---
    struct Joint {
        std::string room;
        double s = 0;  // joint position on the development
        double period = 0;  // development length (perimeter, meters)
        size_t facing_in = 0, facing_out = 0;  // indices into ir.facings
        bool has_tface = false;
        bool wraps = false;  // last -> first facing (zone may cross s = period)
        GridPt sdir;  // T-face only: through-edge walk direction (grid)
        GridPt t_vertex;
        GridPt t_normal;  // outward normal of the T-face (into the room)
        // Concave (270°) contour corner: two open pillar faces look into the
        // room itself. v1 emits no zones here: both flanks always resolve to
        // one style (any room sharing one vertex-adjacent notch atom provably
        // shares the other, so side rules cannot tell the flanks apart).
        bool concave = false;
        GridPt c_normal_in, c_normal_out;  // right(d_in), right(d_out)
    };
    std::vector<Joint> joints;

    for (const auto& fr : frooms) {
        const IrRoom& room = ir.rooms[room_idx[fr.id]];
        // s-coordinate of each contour vertex (walk order; start fixed by the
        // contour itself, so transition ids do not float across style edits).
        std::vector<double> s_vert(fr.grid.size() + 1, 0);
        for (size_t i = 0; i < fr.grid.size(); ++i) {
            const GridPt p = fr.grid[i], q = fr.grid[(i + 1) % fr.grid.size()];
            s_vert[i + 1] = s_vert[i] + (std::abs(q.first - p.first) + std::abs(q.second - p.second)) * cell;
        }
        struct EdgeFacing {
            size_t facing = 0;
            int edge = -1;
        };
        std::vector<EdgeFacing> walk;  // facings in walk order
        for (size_t i = 0; i < fr.grid.size(); ++i) {
            const GridPt p = fr.grid[i], q = fr.grid[(i + 1) % fr.grid.size()];
            const bool vert = p.first == q.first;
            const int w = vert ? ((q.second > p.second) ? 1 : -1) : ((q.first > p.first) ? 1 : -1);
            // Atoms on this contour edge, walk order.
            std::vector<const Atom*> edge_atoms;
            for (const auto& a : atoms) {
                if (a.vert != vert || a.coord != (vert ? p.first : p.second)) continue;
                const int lo = vert ? std::min(p.second, q.second) : std::min(p.first, q.first);
                const int hi = vert ? std::max(p.second, q.second) : std::max(p.first, q.first);
                if (lo <= a.t0 && a.t1 <= hi) edge_atoms.push_back(&a);
            }
            std::sort(edge_atoms.begin(), edge_atoms.end(), [&](const Atom* a, const Atom* b) {
                const double ma = (a->t0 + a->t1) * 0.5, mb = (b->t0 + b->t1) * 0.5;
                return w > 0 ? ma < mb : ma > mb;
            });
            for (size_t k = 0; k < edge_atoms.size(); ++k) {
                const Atom& a = *edge_atoms[k];
                // CW contour: interior on the right of the walk.
                const bool room_on_neg = vert ? (w < 0) : (w > 0);
                const std::string other = room_on_neg ? a.room_pos : a.room_neg;
                const bool outer = other.empty();
                const auto [g0, g1] = atom_ends(a);
                const std::string& wid = atom_wall_id[atom_by_ends.at({g0, g1})];
                const IrWall& wall = ir.walls[wall_idx[wid]];
                IrFacing f;
                f.id = "fac:" + fr.id + ":" + std::to_string(i);
                if (edge_atoms.size() > 1) f.id += "." + std::to_string(k);
                f.wall = wid;
                f.room = fr.id;
                const std::string adj_role = outer ? "" : room_of(other).role;
                // 4.2 side level over the resolved room base style; the chain
                // is the room's style chain plus the fired side rules (F12).
                const SideResolution side = apply_side_rules_prov(project, fr.style, outer, adj_role);
                f.style = side.style;
                f.prov["style"] = ir.rooms[room_idx[fr.id]].prov["style"];
                for (const int idx : side.fired)
                    f.prov["style"].push_back(
                        {"side", side_rule_detail(project, idx),
                         project.fill.side_rules[idx].style});
                // Walk-frame geometry: walk start/end (grid). The side normal
                // points INTO the room (slots §2.3 "наружная нормаль стороны":
                // outward from the wall body) = right of the CW walk.
                const int ws_t = (w > 0) ? a.t0 : a.t1;  // walk-start axis coord
                const int we_t = (w > 0) ? a.t1 : a.t0;
                const GridPt ws = vert ? GridPt{a.coord, ws_t} : GridPt{ws_t, a.coord};
                const GridPt we = vert ? GridPt{a.coord, we_t} : GridPt{we_t, a.coord};
                const double wdx = vert ? 0.0 : w, wdy = vert ? w : 0.0;
                const double nx = wdy, ny = -wdx;  // right of walk (grid math view)
                // Margins are halves of the pillars at the atom's ends; the
                // face plane is offset by the wall's own half-thickness toward
                // the room (the body face the room's interior sees).
                const double t_ws = ir.nodes[node_idx[ws]].thick;
                const double t_we = ir.nodes[node_idx[we]].thick;
                const double s_a0 = s_vert[i] + std::abs(ws_t - (vert ? p.second : p.first)) * cell;
                f.s0 = s_a0 + t_ws / 2.0;
                f.s1 = s_a0 + (a.t1 - a.t0) * cell - t_we / 2.0;
                f.from = {(ws.first * cell + wdx * t_ws / 2.0) + nx * wall.thick / 2.0,
                          (ws.second * cell + wdy * t_ws / 2.0) + ny * wall.thick / 2.0};
                f.to = {(we.first * cell - wdx * t_we / 2.0) + nx * wall.thick / 2.0,
                        (we.second * cell - wdy * t_we / 2.0) + ny * wall.thick / 2.0};
                f.n = {nx, ny};
                f.h = room.h;
                // Defensive: the facing normal must probe into the room
                // (exact for figured rooms, where the bbox center can lie
                // outside).
                const double mx = (f.from.first + f.to.first) * 0.5 / cell + nx * 0.25;
                const double mz = (f.from.second + f.to.second) * 0.5 / cell + ny * 0.25;
                if (!point_in_poly(fr.grid, mx, mz)) {
                    err = err_path + ": internal: facing " + f.id + " normal points outward";
                    return false;
                }
                // Cuts: full door segments on this wall, walk order.
                struct DoorS {
                    double s0 = 0, s1 = 0;
                    const IrDoor* d = nullptr;
                };
                std::vector<DoorS> ds;
                for (const auto& did : wall.doors) {
                    const IrDoor& d = door_by_id(did);
                    const int ddt0 = vert ? d.g0.second : d.g0.first;
                    const int ddt1 = vert ? d.g1.second : d.g1.first;
                    const double sd0 = s_a0 + std::abs(ddt0 - ws_t) * cell;
                    const double sd1 = s_a0 + std::abs(ddt1 - ws_t) * cell;
                    ds.push_back({std::min(sd0, sd1), std::max(sd0, sd1), &d});
                }
                std::sort(ds.begin(), ds.end(),
                          [](const DoorS& a, const DoorS& b) { return a.s0 < b.s0; });
                for (const auto& dd : ds) {
                    // Walk-ordered world endpoints on the face plane.
                    const double l0 = dd.s0 - f.s0, l1 = dd.s1 - f.s0;
                    const double seg_len = std::hypot(f.to.first - f.from.first, f.to.second - f.from.second);
                    const double ux = (f.to.first - f.from.first) / seg_len;
                    const double uz = (f.to.second - f.from.second) / seg_len;
                    IrFacing::Cut c;
                    c.a = {f.from.first + ux * l0, f.from.second + uz * l0};
                    c.b = {f.from.first + ux * l1, f.from.second + uz * l1};
                    c.h = dd.d->h;
                    f.cuts.push_back(c);
                }
                walk.push_back({ir.facings.size(), static_cast<int>(i)});
                ir.facings.push_back(std::move(f));
            }
        }
        // Joints in walk order: between consecutive facings.
        for (size_t j = 0; j < walk.size(); ++j) {
            const size_t fi_in = walk[j].facing;
            const size_t fi_out = walk[(j + 1) % walk.size()].facing;
            const IrFacing& f_in = ir.facings[fi_in];
            const IrFacing& f_out = ir.facings[fi_out];
            Joint jt;
            jt.room = fr.id;
            jt.period = s_vert[fr.grid.size()];
            jt.facing_in = fi_in;
            jt.facing_out = fi_out;
            jt.wraps = (j == walk.size() - 1);
            // Joint vertex: shared atom endpoint. T-face iff the vertex is
            // strictly inside this room's contour edge (then both atoms are on
            // the same contour edge).
            if (walk[j].edge == walk[(j + 1) % walk.size()].edge) {
                // Same contour edge: T-vertex between two atoms.
                const GridPt p = fr.grid[walk[j].edge];
                const GridPt q = fr.grid[(walk[j].edge + 1) % fr.grid.size()];
                const bool vert = p.first == q.first;
                // Shared endpoint = f_in's walk-end atom vertex.
                const IrWall& w_in = ir.walls[wall_idx[f_in.wall]];
                // Candidate endpoints; the shared one touches f_out's wall too.
                const IrWall& w_out = ir.walls[wall_idx[f_out.wall]];
                GridPt shared = w_in.g0;
                if (w_out.g0 == w_in.g0 || w_out.g1 == w_in.g0)
                    shared = w_in.g0;
                else
                    shared = w_in.g1;
                jt.t_vertex = shared;
                jt.s = (f_in.s1 + f_out.s0) * 0.5;  // T-face center
                // T-face normal: from the node into this room, probed.
                const GridPt cand[2] = {vert ? GridPt{1, 0} : GridPt{0, 1},
                                        vert ? GridPt{-1, 0} : GridPt{0, -1}};
                GridPt tn{0, 0};
                for (const GridPt cn : cand) {
                    if (point_in_poly(fr.grid, shared.first + 0.5 * cn.first,
                                      shared.second + 0.5 * cn.second))
                        tn = cn;
                }
                if (tn == GridPt{0, 0}) {
                    err = err_path + ": internal: T-face normal unresolved at " + fmt_pt(shared);
                    return false;
                }
                jt.t_normal = tn;
                // Through-edge walk direction (== +s at the joint).
                const int w = vert ? ((q.second > p.second) ? 1 : -1) : ((q.first > p.first) ? 1 : -1);
                jt.sdir = vert ? GridPt{0, w} : GridPt{w, 0};
                jt.has_tface = true;
            } else {
                // Contour corner between edge e and e+1.
                const int corner = (walk[j].edge + 1) % static_cast<int>(fr.grid.size());
                jt.s = s_vert[corner > 0 ? static_cast<size_t>(corner) : fr.grid.size()];
                if (corner == 0) jt.s = s_vert[fr.grid.size()];
                jt.has_tface = false;
                // 270° corner = left turn on the CW walk.
                const GridPt vv = fr.grid[corner];
                const GridPt pp = fr.grid[walk[j].edge];
                const GridPt nn = fr.grid[(corner + 1) % fr.grid.size()];
                const GridPt d_in = unit_dir({vv.first - pp.first, vv.second - pp.second});
                const GridPt d_out = unit_dir({nn.first - vv.first, nn.second - vv.second});
                const long long cross = static_cast<long long>(d_in.first) * d_out.second -
                                        static_cast<long long>(d_in.second) * d_out.first;
                if (cross > 0) {
                    jt.concave = true;
                    jt.t_vertex = vv;
                    jt.c_normal_in = {d_in.second, -d_in.first};   // right of d_in
                    jt.c_normal_out = {d_out.second, -d_out.first};
                }
            }
            joints.push_back(jt);
        }
        // T-face styles + s-intervals are set when transitions are built (below);
        // plain T-faces (no transition) keep the room style from the node pass,
        // fixed here to the incoming facing style for consistency. Concave
        // corner faces take their flanks' styles (equal in v1, see Joint).
        for (const Joint& jt : joints) {
            if (jt.room != fr.id) continue;
            if (jt.has_tface) {
                const std::string& nid = node_id_at.at(jt.t_vertex);
                const auto fit = face_idx.find({nid, jt.t_normal});
                if (fit == face_idx.end()) {
                    err = err_path + ": internal: T-face missing at " + fmt_pt(jt.t_vertex);
                    return false;
                }
                IrNode& node = ir.nodes[node_idx[jt.t_vertex]];
                IrNodeFace& face = node.faces[fit->second];
                if (face.room != fr.id) {
                    err = err_path + ": internal: T-face at " + fmt_pt(jt.t_vertex) +
                          " looks into room " + face.room;
                    return false;
                }
                if (!strictly_inside_edge(fr, jt.t_vertex)) {
                    err = err_path + ": internal: vertex " + fmt_pt(jt.t_vertex) +
                          " not inside room " + fr.id + " edge";
                    return false;
                }
                face.style = ir.facings[jt.facing_in].style;  // A; transitions refine below
                face.prov["style"] = ir.facings[jt.facing_in].prov["style"];
            } else if (jt.concave) {
                const std::string& nid = node_id_at.at(jt.t_vertex);
                IrNode& node = ir.nodes[node_idx[jt.t_vertex]];
                const std::pair<GridPt, size_t> refinements[2] = {
                    {jt.c_normal_in, jt.facing_in},
                    {jt.c_normal_out, jt.facing_out}};
                for (const auto& [nrm, fi] : refinements) {
                    const auto fit = face_idx.find({nid, nrm});
                    if (fit == face_idx.end()) {
                        err = err_path + ": internal: concave face missing at " +
                              fmt_pt(jt.t_vertex);
                        return false;
                    }
                    IrNodeFace& face = node.faces[fit->second];
                    if (face.room != fr.id) {
                        err = err_path + ": internal: concave face at " + fmt_pt(jt.t_vertex) +
                              " looks into room " + face.room;
                        return false;
                    }
                    face.style = ir.facings[fi].style;
                    face.prov["style"] = ir.facings[fi].prov["style"];
                }
            }
        }
    }

    // Transitions: one per joint with differing flank styles (rooms asc, walk order).
    const double zone_w = project.fill.transitions.width;
    const std::string& zone_place = project.fill.transitions.place;
    bool pat_ok = false;
    const int zone_pattern = pattern_code(project.fill.transitions.pattern, pat_ok);
    if (zone_w <= 0) {
        err = err_path + ": fill.transitions.width must be > 0";
        return false;
    }
    for (const Joint& jt : joints) {
        const IrFacing& f_in = ir.facings[jt.facing_in];
        const IrFacing& f_out = ir.facings[jt.facing_out];
        const std::string& style_a = f_in.style;
        const std::string& style_b = f_out.style;
        if (style_a == style_b) continue;  // concave joints: always equal in v1
        IrTransition t;
        t.id = static_cast<int>(ir.transitions.size());
        t.room = jt.room;
        t.style_a = style_a;
        t.style_b = style_b;
        bool sa_ok = false, sb_ok = false;
        const int code_a = style_code(style_a, sa_ok);
        const int code_b = style_code(style_b, sb_ok);
        if (!sa_ok || !sb_ok) {
            err = err_path + ": internal: unresolvable zone styles " + style_a + "|" + style_b;
            return false;
        }
        t.pattern = zone_pattern;
        t.width = zone_w;
        t.place = zone_place;
        t.prov["pattern"] = ProvChain{{"project", "", project.fill.transitions.pattern}};
        t.prov["width"] = ProvChain{{"project", "", fmt_num(zone_w)}};
        t.prov["place"] = ProvChain{{"project", "", zone_place}};
        t.seed = zone_seed(t.id);
        // Unclipped zone on the development.
        double zs0, zs1;
        if (zone_place == "corner") {
            zs0 = jt.s - zone_w / 2.0;
            zs1 = jt.s + zone_w / 2.0;
        } else {  // wall: on the B side, starting at the joint edge
            zs0 = f_out.s0;
            zs1 = f_out.s0 + zone_w;
        }
        // Available runs: involved intervals minus door cuts.
        struct Run {
            double r0 = 0, r1 = 0;
        };
        std::vector<Run> avail;
        // Door s-spans per involved facing (recomputed from door grid spans).
        auto door_spans = [&](size_t fi) {
            std::vector<Run> out;
            const IrFacing& f = ir.facings[fi];
            // Facing frame: l = s - f.s0; recover door s from cut world endpoints.
            const double seg_len = std::hypot(f.to.first - f.from.first, f.to.second - f.from.second);
            const double ux = (f.to.first - f.from.first) / seg_len;
            const double uz = (f.to.second - f.from.second) / seg_len;
            for (const auto& c : f.cuts) {
                const double la = (c.a.first - f.from.first) * ux + (c.a.second - f.from.second) * uz;
                const double lb = (c.b.first - f.from.first) * ux + (c.b.second - f.from.second) * uz;
                out.push_back({f.s0 + std::min(la, lb), f.s0 + std::max(la, lb)});
            }
            return out;
        };
        auto subtract = [](std::vector<Run> base, const std::vector<Run>& holes) {
            for (const auto& h : holes) {
                std::vector<Run> next;
                for (const auto& r : base) {
                    if (h.r1 <= r.r0 + kEps || h.r0 >= r.r1 - kEps) {
                        next.push_back(r);
                        continue;
                    }
                    if (h.r0 > r.r0 + kEps) next.push_back({r.r0, h.r0});
                    if (h.r1 < r.r1 - kEps) next.push_back({h.r1, r.r1});
                }
                base = std::move(next);
            }
            return base;
        };
        if (zone_place == "corner") {
            std::vector<Run> base = {{f_in.s0, f_in.s1}};
            if (jt.has_tface) {
                const double tv = ir.nodes[node_idx[jt.t_vertex]].thick;
                base.push_back({jt.s - tv / 2.0, jt.s + tv / 2.0});
            }
            // Wrap joint: the development is circular; unwrap the outgoing side.
            const double shift = jt.wraps ? jt.period : 0.0;
            base.push_back({f_out.s0 + shift, f_out.s1 + shift});
            auto holes = door_spans(jt.facing_in);
            for (auto h : door_spans(jt.facing_out)) holes.push_back({h.r0 + shift, h.r1 + shift});
            avail = subtract(base, holes);
        } else {
            avail = subtract({{f_out.s0, f_out.s1}}, door_spans(jt.facing_out));
        }
        // Clip the zone to availability.
        std::vector<Run> kept;
        for (const auto& r : avail) {
            const double c0 = std::max(r.r0, zs0), c1 = std::min(r.r1, zs1);
            if (c1 - c0 > kEps) kept.push_back({c0, c1});
        }
        double kept_len = 0;
        for (const auto& r : kept) kept_len += r.r1 - r.r0;
        t.shortened = kept_len < (zs1 - zs0) - kEps;
        if (kept.empty()) {
            t.s0 = t.s1 = jt.s;
            t.shortened = true;
        } else {
            t.s0 = kept.front().r0;
            t.s1 = kept.back().r1;
        }
        if (t.shortened) {
            ir.warnings.push_back("transition " + std::to_string(t.id) + " (room " + t.room + ", " +
                                  style_a + "|" + style_b + "): zone shortened to [" + fmt_num(t.s0) +
                                  ", " + fmt_num(t.s1) + "] (wall too short or a door is in the way)");
        }
        // Pieces: zone runs on each involved unit (t from the ORIGINAL s0).
        auto emit = [&](size_t fi, double l_origin, bool is_face, size_t node_i, size_t face_i,
                        double shift, int flip) {
            const IrFacing& f = ir.facings[fi];
            const double tv = is_face ? ir.nodes[node_idx[jt.t_vertex]].thick : 0.0;
            const double lo = is_face ? jt.s - tv / 2.0 : f.s0 + shift;
            const double hi = is_face ? jt.s + tv / 2.0 : f.s1 + shift;
            for (const auto& r : kept) {
                const double c0 = std::max(r.r0, lo), c1 = std::min(r.r1, hi);
                if (c1 - c0 <= kEps) continue;
                ZonePiece p;
                p.zone = t.id;
                p.pattern = zone_pattern;
                p.seed = t.seed;
                p.t_at_l0 = l_origin - zs0;
                p.flip = flip;
                p.width = zone_w;
                p.module = project.fill.row_module;
                p.l0 = c0 - l_origin;
                p.l1 = c1 - l_origin;
                p.style_a = code_a;
                p.style_b = code_b;
                if (is_face)
                    ir.nodes[node_i].faces[face_i].zones.push_back(p);
                else
                    ir.facings[fi].zones.push_back(p);
            }
        };
        const double out_shift = (jt.wraps && zone_place == "corner") ? jt.period : 0.0;
        emit(jt.facing_in, f_in.s0, false, 0, 0, 0.0, 0);  // facings run with +s
        emit(jt.facing_out, f_out.s0 + out_shift, false, 0, 0, out_shift, 0);
        if (jt.has_tface && zone_place == "corner") {
            const std::string& nid = node_id_at.at(jt.t_vertex);
            const size_t ni = node_idx[jt.t_vertex];
            const size_t fai = face_idx[{nid, jt.t_normal}];
            // Face +x = right of the outward normal (slots §2.4); flip iff it
            // opposes +s (the through-edge walk direction).
            const GridPt right{jt.t_normal.second, -jt.t_normal.first};
            const int flip = (right == jt.sdir) ? 0 : 1;
            emit(jt.facing_out, jt.s, true, ni, fai, 0.0, flip);
        }
        ir.transitions.push_back(std::move(t));
    }

    // Overlap warnings (pathological width vs short walls; v1 warns only).
    {
        std::vector<const IrTransition*> by_s;
        for (const auto& t : ir.transitions) by_s.push_back(&t);
        std::sort(by_s.begin(), by_s.end(), [](const IrTransition* a, const IrTransition* b) {
            if (a->room != b->room) return a->room < b->room;
            return a->s0 < b->s0;
        });
        for (size_t i = 0; i + 1 < by_s.size(); ++i) {
            const auto* a = by_s[i];
            const auto* b = by_s[i + 1];
            if (a->room == b->room && b->s0 < a->s1 - kEps)
                ir.warnings.push_back("transitions " + std::to_string(a->id) + " and " +
                                      std::to_string(b->id) + " (room " + a->room +
                                      ") overlap; narrow fill.transitions.width");
        }
    }

    // Zone pieces ascending by l on every unit (one unit may host runs of
    // several transitions, e.g. both ends of a short facing).
    auto by_l = [](const ZonePiece& a, const ZonePiece& b) {
        if (std::abs(a.l0 - b.l0) > kEps) return a.l0 < b.l0;
        return a.zone < b.zone;
    };
    for (auto& f : ir.facings) std::sort(f.zones.begin(), f.zones.end(), by_l);
    for (auto& n : ir.nodes)
        for (auto& f : n.faces) std::sort(f.zones.begin(), f.zones.end(), by_l);

    // --- 8. derived (§4.3): corridor clear widths (minima enforced at F11) ---
    // Min gap between opposite-facing atoms of the corridor room (the
    // narrowest arm for figured rooms; exactly the min side for rects).
    // Conservative across open notches: a U-shaped corridor reports the notch
    // gap as well.
    for (const auto& fr : frooms) {
        if (!fr.corridor) continue;
        double best = -1;
        for (const auto& a : atoms) {
            const bool a_neg = a.room_neg == fr.id, a_pos = a.room_pos == fr.id;
            if (!a_neg && !a_pos) continue;
            for (const auto& b : atoms) {
                if (b.vert != a.vert || b.coord == a.coord) continue;
                const bool b_neg = b.room_neg == fr.id, b_pos = b.room_pos == fr.id;
                if (!((a_neg && b_pos) || (a_pos && b_neg))) continue;
                const int lo = std::max(a.t0, b.t0), hi = std::min(a.t1, b.t1);
                if (hi <= lo) continue;
                const double gap = std::abs(a.coord - b.coord) * cell;
                if (best < 0 || gap < best) best = gap;
            }
        }
        if (best < 0) {  // degenerate (no opposite pair): bbox fallback
            int x0 = fr.grid[0].first, x1 = x0, y0 = fr.grid[0].second, y1 = y0;
            for (const auto& p : fr.grid) {
                x0 = std::min(x0, p.first);
                x1 = std::max(x1, p.first);
                y0 = std::min(y0, p.second);
                y1 = std::max(y1, p.second);
            }
            best = std::min(x1 - x0, y1 - y0) * cell;
        }
        ir.corridor_clear[fr.id] = best - fr.wall_t;
    }

    // --- 9. stable order (N6) ---
    std::sort(ir.rooms.begin(), ir.rooms.end(), [](const IrRoom& a, const IrRoom& b) { return a.id < b.id; });
    std::sort(ir.walls.begin(), ir.walls.end(),
              [](const IrWall& a, const IrWall& b) { return a.id < b.id; });
    std::sort(ir.facings.begin(), ir.facings.end(),
              [](const IrFacing& a, const IrFacing& b) { return a.id < b.id; });
    std::sort(ir.nodes.begin(), ir.nodes.end(),
              [](const IrNode& a, const IrNode& b) { return a.id < b.id; });
    std::sort(ir.doors.begin(), ir.doors.end(),
              [](const IrDoor& a, const IrDoor& b) { return a.id < b.id; });
    // transitions already in id order; warnings already deterministic.
    return true;
}

}  // namespace

// --- public builders ---------------------------------------------------------

bool build_ir_v2(const std::string& frozen_json, const std::string& frozen_path,
                 const Project& project, const std::string& project_path, IrV2& out,
                 std::string& err) {
    std::vector<FrozenRoom> frooms;
    if (!parse_frozen(frozen_json, frozen_path, frooms, err)) return false;

    std::vector<RoomInput> inputs;
    for (const auto& fr : frooms) {
        RoomInput in;
        in.id = std::to_string(fr.id);  // frozen decimal form
        in.corridor = fr.corridor;
        in.role = room_role(fr.corridor);  // v0 mapping (corridor | hall)
        in.grid = fr.grid;
        // v0 hierarchy: project -> role (4.2); uniform project wall_t.
        const RoleProvenance e = resolve_role_prov(project, in.role);
        in.h = e.entry.h;
        in.style = e.entry.style;
        in.floor_style = e.entry.floor;
        in.ceil_style = e.entry.ceil;
        in.wall_t = project.fill.wall_t;
        in.prov = e.prov;
        // The frozen path forces the uniform project wall_t (ir_v2.md): the
        // chain says where the value actually comes from.
        in.prov["wall_t"] = ProvChain{{"project", "", fmt_num(project.fill.wall_t)}};
        for (const auto& d : fr.doors)
            in.doors.push_back({std::to_string(d.to), d.d0, d.d1, 1});  // always open (§9.1)
        inputs.push_back(std::move(in));
    }

    IrV2 ir;
    ir.frozen_path = frozen_path;
    ir.project_path = project_path;
    if (!build_core(project, inputs, frozen_path, ir, err)) return false;
    out = std::move(ir);
    return true;
}

bool build_ir_from_layout(const LayoutData& layout, const Project& project,
                          const std::string& project_path, IrV2& out, std::string& err) {
    const std::string path = project_path;
    if (!project.layout) {
        err = path + ": project has no layout tier (needs delve-project/1)";
        return false;
    }
    const LayoutParams& lp = *project.layout;
    std::map<std::string, const GraphRoom*> graph;
    for (const auto& r : lp.rooms) graph[r.id] = &r;
    std::map<std::pair<std::string, std::string>, int> passage_dtype;  // unordered pair -> DR_*
    for (const auto& p : lp.passages) {
        bool ok = false;
        const int code = door_code(p.door, ok);
        if (!ok) {
            err = path + ": unknown door type \"" + p.door + "\" (F1 missed it)";
            return false;
        }
        passage_dtype[{std::min(p.a, p.b), std::max(p.a, p.b)}] = code;
    }
    std::map<std::string, const TemplateDecl*> templates;
    for (const auto& t : lp.templates) templates[t.name] = &t;

    // The layout must place exactly the graph rooms (F3 validity covers this
    // for generated layouts; this guards hand-written delve-layout/0 files).
    std::set<std::string> seen;
    for (const auto& lr : layout.rooms) {
        if (!graph.count(lr.id)) {
            err = path + ": layout room \"" + lr.id + "\" is not in the project graph";
            return false;
        }
        seen.insert(lr.id);
    }
    for (const auto& r : lp.rooms)
        if (!seen.count(r.id)) {
            err = path + ": graph room \"" + r.id + "\" is missing from the layout";
            return false;
        }

    std::vector<RoomInput> inputs;
    for (const auto& lr : layout.rooms) {
        const GraphRoom& gr = *graph[lr.id];
        if (lr.role != gr.role) {
            err = path + ": layout room \"" + lr.id + "\": role \"" + lr.role +
                  "\" does not match the graph role \"" + gr.role + "\"";
            return false;
        }
        const bool corridor = gr.role == "corridor";
        if (lr.corridor != corridor) {
            err = path + ": layout room \"" + lr.id + "\": corridor flag does not match the role \"" +
                  gr.role + "\"";
            return false;
        }
        RoomInput in;
        in.id = lr.id;
        in.corridor = lr.corridor;
        in.role = gr.role;
        in.grid = lr.grid;
        if (!check_contour(in.grid, "layout room \"" + lr.id + "\" grid", path, err)) return false;
        if (area2(in.grid) > 0) std::reverse(in.grid.begin(), in.grid.end());  // §5.1
        // 4.2: room -> template -> role -> project. Parametric templates carry
        // no override; unknown template names are treated as parametric.
        const FillOverride* tov = nullptr;
        std::string tmpl_name;
        if (const auto it = templates.find(lr.tmpl); it != templates.end()) {
            tov = &it->second->fill;
            tmpl_name = lr.tmpl;
        }
        const FillOverride* rov = gr.fill.empty() ? nullptr : &gr.fill;
        const ResolvedFill rf = resolve_room_fill(project, gr.role, tov, rov, tmpl_name, gr.id);
        in.h = rf.h;
        in.wall_t = rf.wall_t;
        in.style = rf.style;
        in.floor_style = rf.floor;
        in.ceil_style = rf.ceil;
        in.prov = rf.prov;
        for (const auto& d : lr.doors) {
            if (!graph.count(d.to)) {
                err = path + ": layout room \"" + lr.id + "\": door to \"" + d.to +
                      "\" is not in the project graph";
                return false;
            }
            const auto dit =
                passage_dtype.find({std::min(lr.id, d.to), std::max(lr.id, d.to)});
            if (dit == passage_dtype.end()) {
                err = path + ": layout room \"" + lr.id + "\": door to \"" + d.to +
                      "\" has no passage in the graph";
                return false;
            }
            GridPt p = d.g0, q = d.g1;
            if (q < p) std::swap(p, q);
            if (p.first != q.first && p.second != q.second) {
                err = path + ": layout room \"" + lr.id + "\": door is not axis-aligned";
                return false;
            }
            const int len = std::abs(q.first - p.first) + std::abs(q.second - p.second);
            if (len < 1) {
                err = path + ": layout room \"" + lr.id + "\": zero-length door";
                return false;
            }
            // On the room contour: colinear with a contour edge, within its span.
            bool on_edge = false;
            for (size_t i = 0; i < in.grid.size(); ++i) {
                const GridPt a = in.grid[i], b = in.grid[(i + 1) % in.grid.size()];
                if (a.first == b.first && p.first == q.first && p.first == a.first) {
                    const int lo = std::min(a.second, b.second), hi = std::max(a.second, b.second);
                    if (lo <= p.second && q.second <= hi) on_edge = true;
                }
                if (a.second == b.second && p.second == q.second && p.second == a.second) {
                    const int lo = std::min(a.first, b.first), hi = std::max(a.first, b.first);
                    if (lo <= p.first && q.first <= hi) on_edge = true;
                }
            }
            if (!on_edge) {
                err = path + ": layout room \"" + lr.id + "\": door " + fmt_pt(p) + "-" + fmt_pt(q) +
                      " is not on the room contour";
                return false;
            }
            in.doors.push_back({d.to, p, q, dit->second});
        }
        inputs.push_back(std::move(in));
    }
    // Door pairing: A -> B must be listed back by B -> A with the same segment.
    {
        std::map<std::string, const RoomInput*> by_id;
        for (const auto& r : inputs) by_id[r.id] = &r;
        for (const auto& r : inputs) {
            for (const auto& d : r.doors) {
                bool back = false;
                for (const auto& e : by_id[d.to]->doors)
                    if (e.to == r.id && e.d0 == d.d0 && e.d1 == d.d1) back = true;
                if (!back) {
                    err = path + ": layout room \"" + r.id + "\": door " + fmt_pt(d.d0) + "-" +
                          fmt_pt(d.d1) + " to room \"" + d.to +
                          "\" has no matching entry in room \"" + d.to + "\"";
                    return false;
                }
            }
        }
    }

    IrV2 ir;
    ir.from_layout = true;
    ir.layout_project = layout.source_project;
    ir.layout_seed = layout.source_seed;
    ir.project_path = project_path;
    if (!build_core(project, inputs, path, ir, err)) return false;
    out = std::move(ir);
    return true;
}

// --- F5 artifact -------------------------------------------------------------

bool write_ir_v2_json(const IrV2& ir, std::string& text_out, std::string& err) {
    (void)err;
    auto j_room = [](const std::string& id) {
        return id.empty() ? nlohmann::ordered_json(nullptr) : nlohmann::ordered_json(id);
    };
    // F12: provenance chains, emitted only when present (additive; /2 files
    // and older readers simply lack/ignore the key).
    auto jprov = [](const std::map<std::string, ProvChain>& prov) {
        nlohmann::ordered_json j = nlohmann::ordered_json::object();
        for (const auto& [field, chain] : prov) {
            nlohmann::ordered_json steps = nlohmann::ordered_json::array();
            for (const auto& s : chain)
                steps.push_back({{"level", s.level}, {"detail", s.detail}, {"value", s.value}});
            j[field] = std::move(steps);
        }
        return j;
    };
    nlohmann::ordered_json doc;
    doc["format"] = kIrFormat;
    if (ir.from_layout)
        doc["source"] = {{"layout", {{"project", ir.layout_project}, {"seed", ir.layout_seed}}},
                         {"project", ir.project_path}};
    else
        doc["source"] = {{"frozen", ir.frozen_path}, {"project", ir.project_path}};
    nlohmann::ordered_json jrooms = nlohmann::ordered_json::array();
    for (const auto& r : ir.rooms) {
        nlohmann::ordered_json jgrid = nlohmann::ordered_json::array();
        for (const auto& p : r.grid) jgrid.push_back({p.first, p.second});
        nlohmann::ordered_json jr = {{"id", r.id},
                                     {"corridor", r.corridor},
                                     {"role", r.role},
                                     {"grid", std::move(jgrid)},
                                     {"h", r.h},
                                     {"style", r.style},
                                     {"floor", r.floor_style},
                                     {"ceil", r.ceil_style}};
        if (!r.prov.empty()) jr["prov"] = jprov(r.prov);
        jrooms.push_back(std::move(jr));
    }
    doc["rooms"] = std::move(jrooms);
    nlohmann::ordered_json jwalls = nlohmann::ordered_json::array();
    for (const auto& w : ir.walls) {
        nlohmann::ordered_json jdoors = nlohmann::ordered_json::array();
        for (const auto& d : w.doors) jdoors.push_back(d);
        jwalls.push_back({{"id", w.id},
                          {"outer", w.outer},
                          {"owner", w.owner},
                          {"rooms", {j_room(w.room_left), j_room(w.room_right)}},
                          {"axis", {{w.g0.first, w.g0.second}, {w.g1.first, w.g1.second}}},
                          {"thick", w.thick},
                          {"thick_ends", {w.t_end0, w.t_end1}},
                          {"h", {w.h_left, w.h_right}},
                          {"doors", std::move(jdoors)}});
    }
    doc["walls"] = std::move(jwalls);
    auto jpiece = [](const ZonePiece& p) {
        return nlohmann::ordered_json{{"zone", p.zone}, {"pattern", p.pattern}, {"seed", p.seed},
                                      {"t_at_l0", p.t_at_l0}, {"flip", p.flip}, {"width", p.width},
                                      {"module", p.module}, {"l", {p.l0, p.l1}},
                                      {"styles", {p.style_a, p.style_b}}};
    };
    nlohmann::ordered_json jfac = nlohmann::ordered_json::array();
    for (const auto& f : ir.facings) {
        nlohmann::ordered_json jcuts = nlohmann::ordered_json::array();
        for (const auto& c : f.cuts)
            jcuts.push_back({{"seg", {{c.a.first, c.a.second}, {c.b.first, c.b.second}}}, {"h", c.h}});
        nlohmann::ordered_json jzones = nlohmann::ordered_json::array();
        for (const auto& z : f.zones) jzones.push_back(jpiece(z));
        nlohmann::ordered_json jf = {{"id", f.id},
                                     {"wall", f.wall},
                                     {"room", f.room},
                                     {"style", f.style},
                                     {"seg", {{f.from.first, f.from.second}, {f.to.first, f.to.second}}},
                                     {"n", {f.n.first, f.n.second}},
                                     {"h", f.h},
                                     {"cuts", std::move(jcuts)},
                                     {"zones", std::move(jzones)},
                                     {"s", {f.s0, f.s1}}};
        if (!f.prov.empty()) jf["prov"] = jprov(f.prov);
        jfac.push_back(std::move(jf));
    }
    doc["facings"] = std::move(jfac);
    nlohmann::ordered_json jnodes = nlohmann::ordered_json::array();
    for (const auto& n : ir.nodes) {
        nlohmann::ordered_json jfaces = nlohmann::ordered_json::array();
        for (const auto& f : n.faces) {
            nlohmann::ordered_json jzones = nlohmann::ordered_json::array();
            for (const auto& z : f.zones) jzones.push_back(jpiece(z));
            nlohmann::ordered_json jf = {{"center", {f.center.first, f.center.second}},
                                         {"n", {f.n.first, f.n.second}},
                                         {"room", j_room(f.room)},
                                         {"h", f.h},
                                         {"style", f.style},
                                         {"zones", std::move(jzones)}};
            if (!f.prov.empty()) jf["prov"] = jprov(f.prov);
            jfaces.push_back(std::move(jf));
        }
        jnodes.push_back({{"id", n.id},
                          {"owner", n.owner},
                          {"at", {n.at.first, n.at.second}},
                          {"thick", n.thick},
                          {"h_pillar", n.h_pillar},
                          {"faces", std::move(jfaces)}});
    }
    doc["nodes"] = std::move(jnodes);
    nlohmann::ordered_json jdoors = nlohmann::ordered_json::array();
    for (const auto& d : ir.doors) {
        nlohmann::ordered_json jd = {{"id", d.id},
                                     {"rooms", {d.room_a, d.room_b}},
                                     {"wall", d.wall},
                                     {"grid", {{d.g0.first, d.g0.second}, {d.g1.first, d.g1.second}}},
                                     {"clear_seg", {{d.from.first, d.from.second}, {d.to.first, d.to.second}}},
                                     {"clear", d.clear},
                                     {"h", d.h},
                                     {"frame", d.frame},
                                     {"thick", d.thick},
                                     {"dtype", d.dtype}};
        if (!d.prov.empty()) jd["prov"] = jprov(d.prov);
        jdoors.push_back(std::move(jd));
    }
    doc["doors"] = std::move(jdoors);
    nlohmann::ordered_json jtrans = nlohmann::ordered_json::array();
    for (const auto& t : ir.transitions) {
        nlohmann::ordered_json jt = {{"id", t.id},
                                     {"room", t.room},
                                     {"styles", {t.style_a, t.style_b}},
                                     {"pattern", t.pattern},
                                     {"width", t.width},
                                     {"place", t.place},
                                     {"s", {t.s0, t.s1}},
                                     {"seed", t.seed},
                                     {"shortened", t.shortened}};
        if (!t.prov.empty()) jt["prov"] = jprov(t.prov);
        jtrans.push_back(std::move(jt));
    }
    doc["transitions"] = std::move(jtrans);
    nlohmann::ordered_json jwarn = nlohmann::ordered_json::array();
    for (const auto& w : ir.warnings) jwarn.push_back(w);
    doc["warnings"] = std::move(jwarn);
    nlohmann::ordered_json jclear = nlohmann::ordered_json::object();
    for (const auto& [id, v] : ir.corridor_clear) jclear[id] = v;
    doc["derived"] = {{"corridor_clear", std::move(jclear)}};
    text_out = doc.dump(1) + "\n";
    return true;
}

namespace {

bool j_grid_pt(const nlohmann::json& j, GridPt& out) {
    if (!j.is_array() || j.size() != 2 || !j[0].is_number_integer() || !j[1].is_number_integer())
        return false;
    out = {j[0].get<int>(), j[1].get<int>()};
    return true;
}

bool j_world_pt(const nlohmann::json& j, WorldPt& out) {
    if (!j.is_array() || j.size() != 2 || !j[0].is_number() || !j[1].is_number()) return false;
    out = {j[0].get<double>(), j[1].get<double>()};
    return true;
}

bool j_seg(const nlohmann::json& j, WorldPt& a, WorldPt& b) {
    if (!j.is_array() || j.size() != 2) return false;
    return j_world_pt(j[0], a) && j_world_pt(j[1], b);
}

bool j_piece(const nlohmann::json& j, ZonePiece& p) {    if (!j.is_object()) return false;
    try {
        p.zone = j.at("zone").get<int>();
        p.pattern = j.at("pattern").get<int>();
        p.seed = j.at("seed").get<int>();
        p.t_at_l0 = j.at("t_at_l0").get<double>();
        p.flip = j.at("flip").get<int>();
        p.width = j.at("width").get<double>();
        p.module = j.at("module").get<double>();
        const auto& l = j.at("l");
        if (!l.is_array() || l.size() != 2) return false;
        p.l0 = l[0].get<double>();
        p.l1 = l[1].get<double>();
        p.style_a = j.at("styles").at(0).get<int>();
        p.style_b = j.at("styles").at(1).get<int>();
    } catch (...) {
        return false;
    }
    return true;
}

// Room reference: string id, null = void.
bool j_room_ref(const nlohmann::json& j, std::string& out) {
    if (j.is_null()) {
        out.clear();
        return true;
    }
    if (!j.is_string()) return false;
    out = j.get<std::string>();
    return true;
}

// F12: optional "prov" object (additive; absent -> empty chains).
void j_prov(const nlohmann::json& j, std::map<std::string, ProvChain>& out) {
    out.clear();
    if (!j.contains("prov")) return;
    for (const auto& [field, steps] : j.at("prov").items())
        for (const auto& s : steps)
            out[field].push_back(ProvStep{s.at("level").get<std::string>(),
                                          s.at("detail").get<std::string>(),
                                          s.at("value").get<std::string>()});
}

}  // namespace

bool read_ir_v2_json(const std::string& text, IrV2& out, std::string& err) {
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(text);
    } catch (const std::exception& e) {
        err = std::string("invalid JSON: ") + e.what();
        return false;
    }
    const std::string format = doc.value("format", std::string{});
    if (format != kIrFormat && format != kIrFormatV2) {
        err = "unsupported IR format \"" + format + "\" (expected " + kIrFormat + " or " +
              kIrFormatV2 +
              "); regenerate the IR from the frozen IR / project with the D2 builder";
        if (format == "delve-ir/1")
            err += " (v1 files carry int room ids and cannot be read as v2+)";
        return false;
    }
    try {
        IrV2 ir;
        const auto& src = doc.at("source");
        if (src.contains("frozen")) {
            ir.frozen_path = src.at("frozen").get<std::string>();
            ir.from_layout = false;
        } else if (src.contains("layout")) {
            ir.from_layout = true;
            ir.layout_project = src.at("layout").at("project").get<std::string>();
            ir.layout_seed = src.at("layout").at("seed").get<int>();
        } else {
            throw std::runtime_error("source: expected {frozen, project} or {layout, project}");
        }
        ir.project_path = src.at("project").get<std::string>();
        for (const auto& jr : doc.at("rooms")) {
            IrRoom r;
            r.id = jr.at("id").get<std::string>();
            r.corridor = jr.at("corridor").get<bool>();
            r.role = jr.at("role").get<std::string>();
            for (const auto& jp : jr.at("grid")) {
                GridPt p;
                if (!j_grid_pt(jp, p)) throw std::runtime_error("bad room grid");
                r.grid.push_back(p);
            }
            r.h = jr.at("h").get<double>();
            r.style = jr.at("style").get<std::string>();
            r.floor_style = jr.at("floor").get<std::string>();
            r.ceil_style = jr.at("ceil").get<std::string>();
            j_prov(jr, r.prov);
            ir.rooms.push_back(std::move(r));
        }
        for (const auto& jw : doc.at("walls")) {
            IrWall w;
            w.id = jw.at("id").get<std::string>();
            w.outer = jw.at("outer").get<bool>();
            w.owner = jw.at("owner").get<std::string>();
            if (!j_room_ref(jw.at("rooms").at(0), w.room_left) ||
                !j_room_ref(jw.at("rooms").at(1), w.room_right))
                throw std::runtime_error("bad wall rooms");
            GridPt a, b;
            if (!j_grid_pt(jw.at("axis").at(0), a) || !j_grid_pt(jw.at("axis").at(1), b))
                throw std::runtime_error("bad wall axis");
            w.g0 = a;
            w.g1 = b;
            w.thick = jw.at("thick").get<double>();
            if (jw.contains("thick_ends")) {
                w.t_end0 = jw.at("thick_ends").at(0).get<double>();
                w.t_end1 = jw.at("thick_ends").at(1).get<double>();
            } else {
                w.t_end0 = w.t_end1 = w.thick;  // pre-D2.3b files: uniform thickness
            }
            w.h_left = jw.at("h").at(0).get<double>();
            w.h_right = jw.at("h").at(1).get<double>();
            for (const auto& jd : jw.at("doors")) w.doors.push_back(jd.get<std::string>());
            ir.walls.push_back(std::move(w));
        }
        for (const auto& jf : doc.at("facings")) {
            IrFacing f;
            f.id = jf.at("id").get<std::string>();
            f.wall = jf.at("wall").get<std::string>();
            f.room = jf.at("room").get<std::string>();
            f.style = jf.at("style").get<std::string>();
            if (!j_seg(jf.at("seg"), f.from, f.to)) throw std::runtime_error("bad facing seg");
            if (!j_world_pt(jf.at("n"), f.n)) throw std::runtime_error("bad facing n");
            f.h = jf.at("h").get<double>();
            for (const auto& jc : jf.at("cuts")) {
                IrFacing::Cut c;
                if (!j_seg(jc.at("seg"), c.a, c.b)) throw std::runtime_error("bad cut");
                c.h = jc.at("h").get<double>();
                f.cuts.push_back(c);
            }
            for (const auto& jz : jf.at("zones")) {
                ZonePiece p;
                if (!j_piece(jz, p)) throw std::runtime_error("bad zone piece");
                f.zones.push_back(p);
            }
            f.s0 = jf.at("s").at(0).get<double>();
            f.s1 = jf.at("s").at(1).get<double>();
            j_prov(jf, f.prov);
            ir.facings.push_back(std::move(f));
        }
        for (const auto& jn : doc.at("nodes")) {
            IrNode n;
            n.id = jn.at("id").get<std::string>();
            n.owner = jn.at("owner").get<std::string>();
            if (!j_grid_pt(jn.at("at"), n.at)) throw std::runtime_error("bad node at");
            n.thick = jn.at("thick").get<double>();
            n.h_pillar = jn.at("h_pillar").get<double>();
            for (const auto& jf : jn.at("faces")) {
                IrNodeFace f;
                if (!j_world_pt(jf.at("center"), f.center)) throw std::runtime_error("bad face center");
                if (!j_world_pt(jf.at("n"), f.n)) throw std::runtime_error("bad face n");
                if (!j_room_ref(jf.at("room"), f.room)) throw std::runtime_error("bad face room");
                f.h = jf.at("h").get<double>();
                f.style = jf.at("style").get<std::string>();
                for (const auto& jz : jf.at("zones")) {
                    ZonePiece p;
                    if (!j_piece(jz, p)) throw std::runtime_error("bad zone piece");
                    f.zones.push_back(p);
                }
                j_prov(jf, f.prov);
                n.faces.push_back(std::move(f));
            }
            ir.nodes.push_back(std::move(n));
        }
        for (const auto& jd : doc.at("doors")) {
            IrDoor d;
            d.id = jd.at("id").get<std::string>();
            d.room_a = jd.at("rooms").at(0).get<std::string>();
            d.room_b = jd.at("rooms").at(1).get<std::string>();
            d.wall = jd.at("wall").get<std::string>();
            GridPt a, b;
            if (!j_grid_pt(jd.at("grid").at(0), a) || !j_grid_pt(jd.at("grid").at(1), b))
                throw std::runtime_error("bad door grid");
            d.g0 = a;
            d.g1 = b;
            if (!j_seg(jd.at("clear_seg"), d.from, d.to)) throw std::runtime_error("bad door seg");
            d.clear = jd.at("clear").get<double>();
            d.h = jd.at("h").get<double>();
            d.frame = jd.at("frame").get<double>();
            d.thick = jd.at("thick").get<double>();
            d.dtype = jd.at("dtype").get<int>();
            j_prov(jd, d.prov);
            ir.doors.push_back(std::move(d));
        }
        for (const auto& jt : doc.at("transitions")) {
            IrTransition t;
            t.id = jt.at("id").get<int>();
            t.room = jt.at("room").get<std::string>();
            t.style_a = jt.at("styles").at(0).get<std::string>();
            t.style_b = jt.at("styles").at(1).get<std::string>();
            t.pattern = jt.at("pattern").get<int>();
            t.width = jt.at("width").get<double>();
            t.place = jt.at("place").get<std::string>();
            t.s0 = jt.at("s").at(0).get<double>();
            t.s1 = jt.at("s").at(1).get<double>();
            t.seed = jt.at("seed").get<int>();
            t.shortened = jt.at("shortened").get<bool>();
            j_prov(jt, t.prov);
            ir.transitions.push_back(std::move(t));
        }
        for (const auto& jw : doc.at("warnings")) ir.warnings.push_back(jw.get<std::string>());
        for (const auto& [k, v] : doc.at("derived").at("corridor_clear").items())
            ir.corridor_clear[k] = v.get<double>();
        out = std::move(ir);
    } catch (const std::exception& e) {
        err = std::string("bad IR v2 JSON: ") + e.what();
        return false;
    }
    return true;
}

}  // namespace delve
