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

struct FrozenDoor {
    int to = -1;
    GridPt d0, d1;  // lex-min first
};

struct FrozenRoom {
    int id = 0;
    bool corridor = false;
    std::vector<GridPt> grid;  // normalized: area2 < 0
    std::vector<FrozenDoor> doors;
    int x0 = 0, x1 = 0, y0 = 0, y1 = 0;  // rect bounds (v1: rects only)
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
            err = path + ": " + where + ": figured rooms land at D2 (v1 wants rects, got " +
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
        r.x0 = r.x1 = r.grid[0].first;
        r.y0 = r.y1 = r.grid[0].second;
        for (const auto& p : r.grid) {
            r.x0 = std::min(r.x0, p.first);
            r.x1 = std::max(r.x1, p.first);
            r.y0 = std::min(r.y0, p.second);
            r.y1 = std::max(r.y1, p.second);
        }
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
                    err = path + ": " + where + ": multi-cell doors land at D2 (got length " +
                          std::to_string(len) + ")";
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

// Contour edge of a rect room: axis line (vert, coord), span [t0, t1], t0 < t1.
struct Edge {
    int room = -1;
    int index = -1;  // contour edge index (leaves vertex `index`)
    bool vert = false;
    int coord = 0;
    int t0 = 0, t1 = 0;
};

struct Atom {
    bool vert = false;
    int coord = 0;
    int t0 = 0, t1 = 0;
    int room_neg = -1, room_pos = -1;  // room on each side (-1 = void)
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

bool build_ir_v1(const std::string& frozen_json, const std::string& frozen_path,
                 const Project& project, const std::string& project_path, IrV1& out,
                 std::string& err) {
    const double cell = project.fill.cell;
    const double thick = project.fill.wall_t;

    // --- 1. frozen rooms ---
    std::vector<FrozenRoom> frooms;
    if (!parse_frozen(frozen_json, frozen_path, frooms, err)) return false;
    std::map<int, size_t> room_idx;
    for (size_t i = 0; i < frooms.size(); ++i) room_idx[frooms[i].id] = i;

    IrV1 ir;
    ir.frozen_path = frozen_path;
    ir.project_path = project_path;

    // --- 2. rooms + roles ---
    for (const auto& fr : frooms) {
        IrRoom r;
        r.id = fr.id;
        r.corridor = fr.corridor;
        r.role = room_role(fr.corridor);
        r.grid = fr.grid;
        const RoleEntry e = resolve_role(project, r.role);
        r.h = e.h;
        r.style = e.style;
        r.floor_style = e.floor;
        r.ceil_style = e.ceil;
        ir.rooms.push_back(std::move(r));
    }

    // --- 3. atomize (§5.2 T-rule: every vertex on an edge splits it) ---
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
            } else {
                e.vert = false;
                e.coord = p.second;
                e.t0 = std::min(p.first, q.first);
                e.t1 = std::max(p.first, q.first);
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
            std::set<int> neg, pos;
            for (const auto& e : edges) {
                if (!(e.t0 <= t0 && t1 <= e.t1)) continue;
                const FrozenRoom& fr = frooms[room_idx[e.room]];
                if (key.vert) {
                    if (fr.x1 == key.coord)
                        neg.insert(e.room);
                    else if (fr.x0 == key.coord)
                        pos.insert(e.room);
                    else {
                        err = frozen_path + ": internal: room " + std::to_string(e.room) +
                              " not adjacent to its edge";
                        return false;
                    }
                } else {
                    if (fr.y1 == key.coord)
                        neg.insert(e.room);
                    else if (fr.y0 == key.coord)
                        pos.insert(e.room);
                    else {
                        err = frozen_path + ": internal: room " + std::to_string(e.room) +
                              " not adjacent to its edge";
                        return false;
                    }
                }
            }
            if (neg.size() > 1 || pos.size() > 1) {
                err = frozen_path + ": rooms overlap on " + std::string(key.vert ? "x=" : "y=") +
                      std::to_string(key.coord) + " [" + std::to_string(t0) + "," + std::to_string(t1) +
                      "]";
                return false;
            }
            Atom a;
            a.vert = key.vert;
            a.coord = key.coord;
            a.t0 = t0;
            a.t1 = t1;
            a.room_neg = neg.empty() ? -1 : *neg.begin();
            a.room_pos = pos.empty() ? -1 : *pos.begin();
            if (a.room_neg < 0 && a.room_pos < 0) continue;  // uncovered gap (cannot happen)
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
    auto wall_id_of = [](GridPt g0, GridPt g1) {
        return "wall:" + std::to_string(g0.first) + "," + std::to_string(g0.second) + "-" +
               std::to_string(g1.first) + "," + std::to_string(g1.second);
    };

    std::map<std::string, size_t> wall_idx;
    for (const auto& a : atoms) {
        const auto [g0, g1] = atom_ends(a);
        IrWall w;
        w.id = wall_id_of(g0, g1);
        if (wall_idx.count(w.id)) {
            err = frozen_path + ": internal: duplicate wall " + w.id;
            return false;
        }
        // Rooms left/right of the g0 -> g1 axis (grid math view, x right, y up).
        if (a.vert) {  // axis +y: left = -x = neg side
            w.room_left = a.room_neg;
            w.room_right = a.room_pos;
        } else {  // axis +x: left = +y = pos side
            w.room_left = a.room_pos;
            w.room_right = a.room_neg;
        }
        w.outer = (w.room_left < 0) != (w.room_right < 0);
        if (w.room_left < 0 && w.room_right < 0) {
            err = frozen_path + ": internal: wall " + w.id + " has no rooms";
            return false;
        }
        w.owner = w.outer ? std::max(w.room_left, w.room_right)
                          : std::min(w.room_left, w.room_right);
        w.g0 = g0;
        w.g1 = g1;
        w.thick = thick;
        const double h_owner = ir.rooms[room_idx[w.owner]].h;
        w.h_left = w.room_left < 0 ? h_owner : ir.rooms[room_idx[w.room_left]].h;
        w.h_right = w.room_right < 0 ? h_owner : ir.rooms[room_idx[w.room_right]].h;
        wall_idx[w.id] = ir.walls.size();
        ir.walls.push_back(std::move(w));
    }

    // --- 4. doors -> walls + 5.4 structural checks ---
    std::set<std::tuple<int, int, GridPt, GridPt>> door_pairs;
    for (const auto& fr : frooms)
        for (const auto& d : fr.doors)
            door_pairs.insert({std::min(fr.id, d.to), std::max(fr.id, d.to), d.d0, d.d1});
    for (const auto& [ra, rb, dd0, dd1] : door_pairs) {
        const bool vert = dd0.first == dd1.first;
        const int coord = vert ? dd0.first : dd0.second;
        const int td0 = vert ? dd0.second : dd0.first;
        const int td1 = vert ? dd1.second : dd1.first;
        // Crossing check: a split strictly inside the door span.
        const auto lit = lines.find({vert, coord});
        if (lit == lines.end()) {
            err = frozen_path + ": internal: door of rooms " + std::to_string(ra) + "-" +
                  std::to_string(rb) + " is on no line";
            return false;
        }
        for (const auto& e : lit->second) {
            if ((td0 < e.t0 && e.t0 < td1) || (td0 < e.t1 && e.t1 < td1)) {
                err = frozen_path + ": door " + fmt_pt(dd0) + "-" + fmt_pt(dd1) + " of rooms " +
                      std::to_string(ra) + "-" + std::to_string(rb) +
                      " crosses a T-junction (vertices split walls, doors cannot span them)";
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
            err = frozen_path + ": door " + fmt_pt(dd0) + "-" + fmt_pt(dd1) + " of rooms " +
                  std::to_string(ra) + "-" + std::to_string(rb) + " lies on no wall";
            return false;
        }
        const auto [g0, g1] = atom_ends(*owner_atom);
        const std::string wid = wall_id_of(g0, g1);
        IrWall& wall = ir.walls[wall_idx[wid]];
        const int wa = std::min(wall.room_left, wall.room_right);
        const int wb = std::max(wall.room_left, wall.room_right);
        if (wall.outer || wa != ra || wb != rb) {
            err = frozen_path + ": door " + fmt_pt(dd0) + "-" + fmt_pt(dd1) + " of rooms " +
                  std::to_string(ra) + "-" + std::to_string(rb) + " is not on their shared wall (" +
                  wid + ")";
            return false;
        }
        IrDoor door;
        door.id = "door:" + std::to_string(ra) + "-" + std::to_string(rb);
        door.room_a = ra;
        door.room_b = rb;
        door.wall = wid;
        door.g0 = dd0;
        door.g1 = dd1;
        door.h = project.fill.door_h;
        door.frame = project.fill.frame;
        door.thick = wall.thick;
        door.clear = 1.0 * cell - 2.0 * door.frame;  // v1: 1-cell doors (§5.3)
        if (!(door.clear > 0.0)) {
            err = frozen_path + ": door " + door.id + ": clear opening " + fmt_num(door.clear) +
                  "m <= 0 (door_len * cell - 2 * frame; widen cell or narrow frame)";
            return false;
        }
        // 5.4: door offset from the corner (cells * cell >= thick/2 + frame).
        const double off_cells = std::min(td0 - owner_atom->t0, owner_atom->t1 - td1);
        if (off_cells * cell < thick / 2.0 + door.frame - kEps) {
            err = frozen_path + ": door " + door.id + ": offset from the corner " +
                  fmt_num(off_cells * cell) + "m < thick/2 + frame (" + fmt_num(thick / 2.0) + " + " +
                  fmt_num(door.frame) + ") [5.4]";
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

    // --- 5. nodes + open faces ---
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
            err = frozen_path + ": internal: vertex " + fmt_pt(v) + " has " +
                  std::to_string(incident.size()) + " walls";
            return false;
        }
        std::set<int> adj;
        for (const auto* a : incident) {
            if (a->room_neg >= 0) adj.insert(a->room_neg);
            if (a->room_pos >= 0) adj.insert(a->room_pos);
        }
        IrNode node;
        node.id = "node:" + std::to_string(v.first) + "," + std::to_string(v.second);
        node.owner = *adj.begin();
        node.at = v;
        node.thick = thick;
        node.h_pillar = 0;
        for (int r : adj) node.h_pillar = std::max(node.h_pillar, ir.rooms[room_idx[r]].h);
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
            // Open face: the room strictly beyond it (void if none).
            std::vector<int> beyond;
            for (int r : adj) {
                const FrozenRoom& fr = frooms[room_idx[r]];
                if (n.first == 1 && fr.x0 == v.first && fr.y0 < v.second && v.second < fr.y1)
                    beyond.push_back(r);
                if (n.first == -1 && fr.x1 == v.first && fr.y0 < v.second && v.second < fr.y1)
                    beyond.push_back(r);
                if (n.second == 1 && fr.y0 == v.second && fr.x0 < v.first && v.first < fr.x1)
                    beyond.push_back(r);
                if (n.second == -1 && fr.y1 == v.second && fr.x0 < v.first && v.first < fr.x1)
                    beyond.push_back(r);
            }
            if (beyond.size() > 1) {
                err = frozen_path + ": rooms overlap past vertex " + fmt_pt(v);
                return false;
            }
            IrNodeFace f;
            f.center = {n.first * thick / 2.0, n.second * thick / 2.0};
            f.n = {static_cast<double>(n.first), static_cast<double>(n.second)};
            f.room = beyond.empty() ? -1 : beyond[0];
            if (f.room < 0) {
                f.h = ir.rooms[room_idx[node.owner]].h;
                f.style = resolve_role(project, ir.rooms[room_idx[node.owner]].role).style;
            } else {
                f.h = ir.rooms[room_idx[f.room]].h;
                f.style = ir.rooms[room_idx[f.room]].style;  // T-faces refined at step 7
            }
            node.faces.push_back(std::move(f));
        }
        // Deterministic face order: lex by normal.
        std::sort(node.faces.begin(), node.faces.end(), [](const IrNodeFace& a, const IrNodeFace& b) {
            if (a.n.first != b.n.first) return a.n.first < b.n.first;
            return a.n.second < b.n.second;
        });
        node_idx[v] = ir.nodes.size();
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

    auto strictly_inside_edge = [&](const FrozenRoom& fr, GridPt v) {
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
    };

    // --- 6+7. developments + transitions ---
    struct Joint {
        int room = -1;
        double s = 0;  // joint position on the development
        double period = 0;  // development length (perimeter, meters)
        size_t facing_in = 0, facing_out = 0;  // indices into ir.facings
        bool has_tface = false;
        bool wraps = false;  // last -> first facing (zone may cross s = period)
        GridPt t_vertex;
        GridPt t_normal;  // outward normal of the T-face (into the room)
    };
    std::vector<Joint> joints;

    for (const auto& fr : frooms) {
        const IrRoom& room = ir.rooms[room_idx[fr.id]];
        const double cx = (fr.x0 + fr.x1) * 0.5 * cell, cz = (fr.y0 + fr.y1) * 0.5 * cell;
        // s-coordinate of each contour vertex (walk order).
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
                const bool room_on_neg =
                    (a.vert && ((fr.x1 == a.coord))) || (!a.vert && ((fr.y1 == a.coord)));
                const int other = room_on_neg ? a.room_pos : a.room_neg;
                const bool outer = other < 0;
                const auto [g0, g1] = atom_ends(a);
                const std::string wid = wall_id_of(g0, g1);
                IrFacing f;
                f.id = "fac:" + std::to_string(fr.id) + ":" + std::to_string(i);
                if (edge_atoms.size() > 1) f.id += "." + std::to_string(k);
                f.wall = wid;
                f.room = fr.id;
                const std::string adj_role =
                    outer ? "" : ir.rooms[room_idx[other]].role;
                f.style = resolve_side_style(project, room.role, outer, adj_role);
                // Walk-frame geometry: walk start/end (grid), outward = left of walk.
                const int ws_t = (w > 0) ? a.t0 : a.t1;  // walk-start axis coord
                const int we_t = (w > 0) ? a.t1 : a.t0;
                const GridPt ws = vert ? GridPt{a.coord, ws_t} : GridPt{ws_t, a.coord};
                const GridPt we = vert ? GridPt{a.coord, we_t} : GridPt{we_t, a.coord};
                const double wdx = vert ? 0.0 : w, wdy = vert ? w : 0.0;
                const double nx = -wdy, ny = wdx;  // left of walk (grid math view)
                const double s_a0 = s_vert[i] + std::abs(ws_t - (vert ? p.second : p.first)) * cell;
                f.s0 = s_a0 + thick / 2.0;
                f.s1 = s_a0 + (a.t1 - a.t0) * cell - thick / 2.0;
                f.from = {(ws.first * cell + wdx * thick / 2.0) + nx * thick / 2.0,
                          (ws.second * cell + wdy * thick / 2.0) + ny * thick / 2.0};
                f.to = {(we.first * cell - wdx * thick / 2.0) + nx * thick / 2.0,
                        (we.second * cell - wdy * thick / 2.0) + ny * thick / 2.0};
                f.n = {nx, ny};
                f.h = room.h;
                // Defensive: outward must point away from the room center.
                const double mx = (f.from.first + f.to.first) * 0.5 - cx;
                const double mz = (f.from.second + f.to.second) * 0.5 - cz;
                if (mx * nx + mz * ny <= 0) {
                    err = frozen_path + ": internal: facing " + f.id + " normal points inward";
                    return false;
                }
                // Cuts: full door segments on this wall, walk order.
                const IrWall& wall = ir.walls[wall_idx[wid]];
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
                // T-face normal: from the node into this room.
                const GridPt n = vert ? GridPt{(fr.x0 + fr.x1) / 2 > shared.first ? 1 : -1, 0}
                                      : GridPt{0, (fr.y0 + fr.y1) / 2 > shared.second ? 1 : -1};
                jt.t_normal = n;
                jt.has_tface = true;
                (void)q;
            } else {
                // Contour corner between edge e and e+1.
                const int corner = (walk[j].edge + 1) % static_cast<int>(fr.grid.size());
                jt.s = s_vert[corner > 0 ? static_cast<size_t>(corner) : fr.grid.size()];
                if (corner == 0) jt.s = s_vert[fr.grid.size()];
                jt.has_tface = false;
            }
            joints.push_back(jt);
        }
        // T-face styles + s-intervals are set when transitions are built (below);
        // plain T-faces (no transition) keep the room style from step 5, fixed here
        // to the incoming facing style for consistency.
        for (const Joint& jt : joints) {
            if (jt.room != fr.id || !jt.has_tface) continue;
            const std::string nid =
                "node:" + std::to_string(jt.t_vertex.first) + "," + std::to_string(jt.t_vertex.second);
            const auto fit = face_idx.find({nid, jt.t_normal});
            if (fit == face_idx.end()) {
                err = frozen_path + ": internal: T-face missing at " + fmt_pt(jt.t_vertex);
                return false;
            }
            IrNode& node = ir.nodes[node_idx[jt.t_vertex]];
            IrNodeFace& face = node.faces[fit->second];
            if (face.room != fr.id) {
                err = frozen_path + ": internal: T-face at " + fmt_pt(jt.t_vertex) +
                      " looks into room " + std::to_string(face.room);
                return false;
            }
            if (!strictly_inside_edge(fr, jt.t_vertex)) {
                err = frozen_path + ": internal: vertex " + fmt_pt(jt.t_vertex) + " not inside room " +
                      std::to_string(fr.id) + " edge";
                return false;
            }
            face.style = ir.facings[jt.facing_in].style;  // A; transitions refine below
        }
    }

    // Transitions: one per joint with differing flank styles (rooms asc, walk order).
    const double zone_w = project.fill.transitions.width;
    const std::string& zone_place = project.fill.transitions.place;
    bool pat_ok = false;
    const int zone_pattern = pattern_code(project.fill.transitions.pattern, pat_ok);
    if (zone_w <= 0) {
        err = project_path + ": fill.transitions.width must be > 0";
        return false;
    }
    for (const Joint& jt : joints) {
        const IrFacing& f_in = ir.facings[jt.facing_in];
        const IrFacing& f_out = ir.facings[jt.facing_out];
        const std::string& style_a = f_in.style;
        const std::string& style_b = f_out.style;
        if (style_a == style_b) continue;
        IrTransition t;
        t.id = static_cast<int>(ir.transitions.size());
        t.room = jt.room;
        t.style_a = style_a;
        t.style_b = style_b;
        t.pattern = zone_pattern;
        t.width = zone_w;
        t.place = zone_place;
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
            if (jt.has_tface) base.push_back({jt.s - thick / 2.0, jt.s + thick / 2.0});
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
            ir.warnings.push_back("transition " + std::to_string(t.id) + " (room " +
                                  std::to_string(t.room) + ", " + style_a + "|" + style_b +
                                  "): zone shortened to [" + fmt_num(t.s0) + ", " + fmt_num(t.s1) +
                                  "] (wall too short or a door is in the way)");
        }
        // Pieces: zone runs on each involved unit (t from the ORIGINAL s0).
        auto emit = [&](size_t fi, double l_origin, bool is_face, size_t node_i, size_t face_i,
                        double shift) {
            const IrFacing& f = ir.facings[fi];
            const double lo = is_face ? jt.s - thick / 2.0 : f.s0 + shift;
            const double hi = is_face ? jt.s + thick / 2.0 : f.s1 + shift;
            for (const auto& r : kept) {
                const double c0 = std::max(r.r0, lo), c1 = std::min(r.r1, hi);
                if (c1 - c0 <= kEps) continue;
                ZonePiece p;
                p.zone = t.id;
                p.pattern = zone_pattern;
                p.seed = t.seed;
                p.t_at_l0 = l_origin - zs0;
                p.flip = 0;
                p.width = zone_w;
                p.module = project.fill.row_module;
                p.l0 = c0 - l_origin;
                p.l1 = c1 - l_origin;
                if (is_face)
                    ir.nodes[node_i].faces[face_i].zones.push_back(p);
                else
                    ir.facings[fi].zones.push_back(p);
            }
        };
        const double out_shift = (jt.wraps && zone_place == "corner") ? jt.period : 0.0;
        emit(jt.facing_in, f_in.s0, false, 0, 0, 0.0);
        emit(jt.facing_out, f_out.s0 + out_shift, false, 0, 0, out_shift);
        if (jt.has_tface && zone_place == "corner") {
            const std::string nid = "node:" + std::to_string(jt.t_vertex.first) + "," +
                                    std::to_string(jt.t_vertex.second);
            const size_t ni = node_idx[jt.t_vertex];
            const size_t fai = face_idx[{nid, jt.t_normal}];
            emit(jt.facing_out, jt.s, true, ni, fai, 0.0);
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
                                      std::to_string(b->id) + " (room " + std::to_string(a->room) +
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
    for (const auto& fr : frooms) {
        if (!fr.corridor) continue;
        const double w = (fr.x1 - fr.x0) * cell - thick;
        const double h = (fr.y1 - fr.y0) * cell - thick;
        ir.corridor_clear[fr.id] = std::min(w, h);
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
    out = std::move(ir);
    return true;
}

bool write_ir_v1_json(const IrV1& ir, std::string& text_out, std::string& err) {
    (void)err;
    nlohmann::ordered_json doc;
    doc["format"] = kIrFormat;
    doc["source"] = {{"frozen", ir.frozen_path}, {"project", ir.project_path}};
    nlohmann::ordered_json jrooms = nlohmann::ordered_json::array();
    for (const auto& r : ir.rooms) {
        nlohmann::ordered_json jgrid = nlohmann::ordered_json::array();
        for (const auto& p : r.grid) jgrid.push_back({p.first, p.second});
        jrooms.push_back({{"id", r.id},
                          {"corridor", r.corridor},
                          {"role", r.role},
                          {"grid", std::move(jgrid)},
                          {"h", r.h},
                          {"style", r.style},
                          {"floor", r.floor_style},
                          {"ceil", r.ceil_style}});
    }
    doc["rooms"] = std::move(jrooms);
    nlohmann::ordered_json jwalls = nlohmann::ordered_json::array();
    for (const auto& w : ir.walls) {
        nlohmann::ordered_json jdoors = nlohmann::ordered_json::array();
        for (const auto& d : w.doors) jdoors.push_back(d);
        jwalls.push_back({{"id", w.id},
                          {"outer", w.outer},
                          {"owner", w.owner},
                          {"rooms", {w.room_left, w.room_right}},
                          {"axis", {{w.g0.first, w.g0.second}, {w.g1.first, w.g1.second}}},
                          {"thick", w.thick},
                          {"h", {w.h_left, w.h_right}},
                          {"doors", std::move(jdoors)}});
    }
    doc["walls"] = std::move(jwalls);
    auto jpiece = [](const ZonePiece& p) {
        return nlohmann::ordered_json{{"zone", p.zone}, {"pattern", p.pattern}, {"seed", p.seed},
                                      {"t_at_l0", p.t_at_l0}, {"flip", p.flip}, {"width", p.width},
                                      {"module", p.module}, {"l", {p.l0, p.l1}}};
    };
    nlohmann::ordered_json jfac = nlohmann::ordered_json::array();
    for (const auto& f : ir.facings) {
        nlohmann::ordered_json jcuts = nlohmann::ordered_json::array();
        for (const auto& c : f.cuts)
            jcuts.push_back({{"seg", {{c.a.first, c.a.second}, {c.b.first, c.b.second}}}, {"h", c.h}});
        nlohmann::ordered_json jzones = nlohmann::ordered_json::array();
        for (const auto& z : f.zones) jzones.push_back(jpiece(z));
        jfac.push_back({{"id", f.id},
                        {"wall", f.wall},
                        {"room", f.room},
                        {"style", f.style},
                        {"seg", {{f.from.first, f.from.second}, {f.to.first, f.to.second}}},
                        {"n", {f.n.first, f.n.second}},
                        {"h", f.h},
                        {"cuts", std::move(jcuts)},
                        {"zones", std::move(jzones)},
                        {"s", {f.s0, f.s1}}});
    }
    doc["facings"] = std::move(jfac);
    nlohmann::ordered_json jnodes = nlohmann::ordered_json::array();
    for (const auto& n : ir.nodes) {
        nlohmann::ordered_json jfaces = nlohmann::ordered_json::array();
        for (const auto& f : n.faces) {
            nlohmann::ordered_json jzones = nlohmann::ordered_json::array();
            for (const auto& z : f.zones) jzones.push_back(jpiece(z));
            jfaces.push_back({{"center", {f.center.first, f.center.second}},
                              {"n", {f.n.first, f.n.second}},
                              {"room", f.room},
                              {"h", f.h},
                              {"style", f.style},
                              {"zones", std::move(jzones)}});
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
    for (const auto& d : ir.doors)
        jdoors.push_back({{"id", d.id},
                          {"rooms", {d.room_a, d.room_b}},
                          {"wall", d.wall},
                          {"grid", {{d.g0.first, d.g0.second}, {d.g1.first, d.g1.second}}},
                          {"clear_seg", {{d.from.first, d.from.second}, {d.to.first, d.to.second}}},
                          {"clear", d.clear},
                          {"h", d.h},
                          {"frame", d.frame},
                          {"thick", d.thick},
                          {"dtype", d.dtype}});
    doc["doors"] = std::move(jdoors);
    nlohmann::ordered_json jtrans = nlohmann::ordered_json::array();
    for (const auto& t : ir.transitions)
        jtrans.push_back({{"id", t.id},
                          {"room", t.room},
                          {"styles", {t.style_a, t.style_b}},
                          {"pattern", t.pattern},
                          {"width", t.width},
                          {"place", t.place},
                          {"s", {t.s0, t.s1}},
                          {"seed", t.seed},
                          {"shortened", t.shortened}});
    doc["transitions"] = std::move(jtrans);
    nlohmann::ordered_json jwarn = nlohmann::ordered_json::array();
    for (const auto& w : ir.warnings) jwarn.push_back(w);
    doc["warnings"] = std::move(jwarn);
    nlohmann::ordered_json jclear = nlohmann::ordered_json::object();
    for (const auto& [id, v] : ir.corridor_clear) jclear[std::to_string(id)] = v;
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

bool j_piece(const nlohmann::json& j, ZonePiece& p) {
    if (!j.is_object()) return false;
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
    } catch (...) {
        return false;
    }
    return true;
}

}  // namespace

bool read_ir_v1_json(const std::string& text, IrV1& out, std::string& err) {
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(text);
    } catch (const std::exception& e) {
        err = std::string("invalid JSON: ") + e.what();
        return false;
    }
    if (doc.value("format", std::string{}) != kIrFormat) {
        err = "unsupported IR format \"" + doc.value("format", std::string{}) + "\" (expected " +
              kIrFormat + "); regenerate the IR from the frozen IR with the D1 builder";
        return false;
    }
    try {
        IrV1 ir;
        ir.frozen_path = doc.at("source").at("frozen").get<std::string>();
        ir.project_path = doc.at("source").at("project").get<std::string>();
        for (const auto& jr : doc.at("rooms")) {
            IrRoom r;
            r.id = jr.at("id").get<int>();
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
            ir.rooms.push_back(std::move(r));
        }
        for (const auto& jw : doc.at("walls")) {
            IrWall w;
            w.id = jw.at("id").get<std::string>();
            w.outer = jw.at("outer").get<bool>();
            w.owner = jw.at("owner").get<int>();
            w.room_left = jw.at("rooms").at(0).get<int>();
            w.room_right = jw.at("rooms").at(1).get<int>();
            GridPt a, b;
            if (!j_grid_pt(jw.at("axis").at(0), a) || !j_grid_pt(jw.at("axis").at(1), b))
                throw std::runtime_error("bad wall axis");
            w.g0 = a;
            w.g1 = b;
            w.thick = jw.at("thick").get<double>();
            w.h_left = jw.at("h").at(0).get<double>();
            w.h_right = jw.at("h").at(1).get<double>();
            for (const auto& jd : jw.at("doors")) w.doors.push_back(jd.get<std::string>());
            ir.walls.push_back(std::move(w));
        }
        for (const auto& jf : doc.at("facings")) {
            IrFacing f;
            f.id = jf.at("id").get<std::string>();
            f.wall = jf.at("wall").get<std::string>();
            f.room = jf.at("room").get<int>();
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
            ir.facings.push_back(std::move(f));
        }
        for (const auto& jn : doc.at("nodes")) {
            IrNode n;
            n.id = jn.at("id").get<std::string>();
            n.owner = jn.at("owner").get<int>();
            if (!j_grid_pt(jn.at("at"), n.at)) throw std::runtime_error("bad node at");
            n.thick = jn.at("thick").get<double>();
            n.h_pillar = jn.at("h_pillar").get<double>();
            for (const auto& jf : jn.at("faces")) {
                IrNodeFace f;
                if (!j_world_pt(jf.at("center"), f.center)) throw std::runtime_error("bad face center");
                if (!j_world_pt(jf.at("n"), f.n)) throw std::runtime_error("bad face n");
                f.room = jf.at("room").get<int>();
                f.h = jf.at("h").get<double>();
                f.style = jf.at("style").get<std::string>();
                for (const auto& jz : jf.at("zones")) {
                    ZonePiece p;
                    if (!j_piece(jz, p)) throw std::runtime_error("bad zone piece");
                    f.zones.push_back(p);
                }
                n.faces.push_back(std::move(f));
            }
            ir.nodes.push_back(std::move(n));
        }
        for (const auto& jd : doc.at("doors")) {
            IrDoor d;
            d.id = jd.at("id").get<std::string>();
            d.room_a = jd.at("rooms").at(0).get<int>();
            d.room_b = jd.at("rooms").at(1).get<int>();
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
            ir.doors.push_back(std::move(d));
        }
        for (const auto& jt : doc.at("transitions")) {
            IrTransition t;
            t.id = jt.at("id").get<int>();
            t.room = jt.at("room").get<int>();
            t.style_a = jt.at("styles").at(0).get<std::string>();
            t.style_b = jt.at("styles").at(1).get<std::string>();
            t.pattern = jt.at("pattern").get<int>();
            t.width = jt.at("width").get<double>();
            t.place = jt.at("place").get<std::string>();
            t.s0 = jt.at("s").at(0).get<double>();
            t.s1 = jt.at("s").at(1).get<double>();
            t.seed = jt.at("seed").get<int>();
            t.shortened = jt.at("shortened").get<bool>();
            ir.transitions.push_back(std::move(t));
        }
        for (const auto& jw : doc.at("warnings")) ir.warnings.push_back(jw.get<std::string>());
        for (const auto& [k, v] : doc.at("derived").at("corridor_clear").items())
            ir.corridor_clear[std::stoi(k)] = v.get<double>();
        out = std::move(ir);
    } catch (const std::exception& e) {
        err = std::string("bad IR v1 JSON: ") + e.what();
        return false;
    }
    return true;
}

}  // namespace delve
