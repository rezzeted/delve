#include "pch.h"

#include "generate.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <map>
#include <random>
#include <set>
#include <utility>

#include <nlohmann/json.hpp>

#include "edgar/generator/grid2d/layout_door_computation.hpp"
#include "edgar/generator/grid2d/level_description_grid2d.hpp"
#include "edgar/geometry/overlap.hpp"

namespace delve::layout {
namespace {

namespace grid2d = edgar::generator::grid2d;

// A layout is usable when every graph room is placed exactly once, every
// passage has a door on both sides with the same world segment, and no two
// room outlines overlap by area (touching at shared edges is required).
// Returns an empty string when valid, otherwise a short reason.
std::string valid_layout(const grid2d::LayoutGrid2D<int>& layout, const LayoutParams& l,
                         const std::map<std::string, int>& id_to_index) {
    if (layout.rooms.size() != l.rooms.size())
        return "room count " + std::to_string(layout.rooms.size()) + " != graph " +
               std::to_string(l.rooms.size());
    std::map<int, const grid2d::LayoutRoomGrid2D<int>*> by_idx;
    for (const auto& r : layout.rooms) {
        if (r.room < 0 || static_cast<size_t>(r.room) >= l.rooms.size())
            return "room index out of range: " + std::to_string(r.room);
        if (!by_idx.emplace(r.room, &r).second)
            return "room index placed twice: " + std::to_string(r.room);
    }
    const auto world_segs = [](const grid2d::LayoutRoomGrid2D<int>* r, int from, int to) {
        std::set<std::pair<CellPt, CellPt>> out;
        for (const auto& d : r->doors) {
            if (d.from_room != from || d.to_room != to) continue;
            const CellPt a{d.door_line.from.x + r->position.x, d.door_line.from.y + r->position.y};
            const CellPt b{d.door_line.to.x + r->position.x, d.door_line.to.y + r->position.y};
            out.insert({std::min(a, b), std::max(a, b)});
        }
        return out;
    };
    for (const auto& p : l.passages) {
        const int ia = id_to_index.at(p.a), ib = id_to_index.at(p.b);
        const auto sa = world_segs(by_idx.at(ia), ia, ib);
        const auto sb = world_segs(by_idx.at(ib), ib, ia);
        if (sa.empty() || sb.empty())
            return "passage " + p.a + "-" + p.b + ": missing door side (a=" +
                   std::to_string(sa.size()) + ", b=" + std::to_string(sb.size()) + ")";
        bool share = false;
        for (const auto& s : sa)
            if (sb.count(s)) {
                share = true;
                break;
            }
        if (!share) return "passage " + p.a + "-" + p.b + ": door segments differ";
    }
    for (size_t i = 0; i < layout.rooms.size(); ++i)
        for (size_t j = i + 1; j < layout.rooms.size(); ++j)
            if (edgar::geometry::polygons_overlap_area(layout.rooms[i].outline,
                                                       layout.rooms[i].position,
                                                       layout.rooms[j].outline,
                                                       layout.rooms[j].position))
                return "rooms " + std::to_string(layout.rooms[i].room) + " and " +
                       std::to_string(layout.rooms[j].room) + " overlap by area";
    // Minimum room distance (§9.1): bbox-Manhattan gap on non-neighbour pairs,
    // mirroring edgar's MinimumDistanceConstraint (passage neighbours exempt).
    // Rechecked here so validity never depends on port energy internals.
    if (l.min_room_distance > 0) {
        std::set<std::pair<int, int>> adjacent;
        for (const auto& p : l.passages) {
            const int ia = id_to_index.at(p.a), ib = id_to_index.at(p.b);
            adjacent.insert({std::min(ia, ib), std::max(ia, ib)});
        }
        const auto bbox = [](const grid2d::LayoutRoomGrid2D<int>& r) {
            int sx = INT_MAX, bx = INT_MIN, sy = INT_MAX, by = INT_MIN;
            for (const auto& pt : r.outline.points()) {
                sx = std::min(sx, pt.x + r.position.x);
                bx = std::max(bx, pt.x + r.position.x);
                sy = std::min(sy, pt.y + r.position.y);
                by = std::max(by, pt.y + r.position.y);
            }
            return std::array<int, 4>{sx, bx, sy, by};
        };
        for (size_t i = 0; i < layout.rooms.size(); ++i)
            for (size_t j = i + 1; j < layout.rooms.size(); ++j) {
                const int a = layout.rooms[i].room, b = layout.rooms[j].room;
                if (adjacent.count({std::min(a, b), std::max(a, b)})) continue;
                const auto ra = bbox(layout.rooms[i]), rb = bbox(layout.rooms[j]);
                const int dx = std::max(0, std::max(ra[0] - rb[1], rb[0] - ra[1]));
                const int dy = std::max(0, std::max(ra[2] - rb[3], rb[2] - ra[3]));
                if (dx + dy < l.min_room_distance)
                    return "rooms " + std::to_string(a) + " and " + std::to_string(b) +
                           " closer than min_room_distance " +
                           std::to_string(l.min_room_distance);
            }
    }
    return "";
}

}  // namespace

void LayoutGenerator::request_cancel() {
    std::lock_guard<std::mutex> lock(mu_);
    sticky_cancel_ = true;
    if (cancelable_ && active_) active_->request_cancel();
}

void LayoutGenerator::reset_cancel() {
    std::lock_guard<std::mutex> lock(mu_);
    sticky_cancel_ = false;
}

bool LayoutGenerator::generate(const Project& project, const Catalog& catalog,
                               const GenerateOptions& opts, LayoutResult& out, std::string& err) {
    out = LayoutResult{};
    if (!project.layout) {
        err = "generate: project has no layout tier (needs delve-project/1)";
        return false;
    }
    if (opts.attempts < 1) {
        err = "generate: attempts must be >= 1";
        return false;
    }
    if ((opts.time_budget_ms && *opts.time_budget_ms < 0) ||
        (opts.iteration_budget && *opts.iteration_budget < 0)) {
        err = "generate: budgets must be >= 0";
        return false;
    }
    const LayoutParams& l = *project.layout;
    std::vector<std::string> ids;
    std::map<std::string, int> idx;
    for (const auto& r : l.rooms) {
        idx[r.id] = static_cast<int>(ids.size());
        ids.push_back(r.id);
    }
    std::map<std::string, grid2d::RoomDescriptionGrid2D> desc;
    if (!build_room_descriptions(project, catalog, desc, err)) return false;
    grid2d::LevelDescriptionGrid2D<int> level;
    try {
        for (size_t i = 0; i < ids.size(); ++i) level.add_room(static_cast<int>(i), desc.at(ids[i]));
        for (const auto& p : l.passages) level.add_connection(idx.at(p.a), idx.at(p.b));
    } catch (const std::exception& e) {
        err = std::string("generate: internal: level build failed: ") + e.what();
        return false;
    }
    level.minimum_room_distance = l.min_room_distance;

    const bool has_budgets = opts.time_budget_ms.has_value() || opts.iteration_budget.has_value();
    const int base_seed = layout_seed(project.seed);
    {
        std::lock_guard<std::mutex> lock(mu_);
        cancelable_ = !has_budgets;
        if (sticky_cancel_) {
            err = "generate: cancelled";
            return false;
        }
    }
    std::string last_diag = "no attempts ran";
    for (int attempt = 0; attempt < opts.attempts; ++attempt) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (sticky_cancel_) {
                err = "generate: cancelled";
                return false;
            }
        }
        const int seed_a = static_cast<int>((static_cast<unsigned>(base_seed) +
                                             static_cast<unsigned>(attempt)) &
                                            0x7fffffff);
        grid2d::GraphBasedGeneratorConfiguration cfg;
        if (opts.time_budget_ms)
            cfg.early_stop_max_elapsed = std::chrono::milliseconds(*opts.time_budget_ms);
        if (opts.iteration_budget)
            cfg.early_stop_max_total_iterations = *opts.iteration_budget;
        grid2d::GraphBasedGeneratorGrid2D<int> gen(level, cfg);
        gen.inject_random_generator(std::mt19937(static_cast<unsigned>(seed_a)));
        grid2d::LayoutGrid2D<int> layout;
        {
            std::lock_guard<std::mutex> lock(mu_);
            active_ = &gen;
        }
        bool threw = false;
        std::string what;
        try {
            layout = gen.generate_layout();
        } catch (const std::exception& e) {
            threw = true;
            what = e.what();
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            active_ = nullptr;
            if (sticky_cancel_) {
                err = "generate: cancelled";
                return false;
            }
        }
        out.time_ms = gen.time_total_ms();
        out.iterations = gen.iterations_count();
        if (threw) {
            last_diag = std::string("attempt ") + std::to_string(attempt) + " threw: " + what;
            continue;
        }
        try {
            std::mt19937 door_rng(static_cast<unsigned>(seed_a) + 0x9e37u);
            auto graph = level.get_graph();
            grid2d::compute_layout_doors(layout, level, graph, door_rng);
        } catch (const std::exception& e) {
            last_diag =
                std::string("attempt ") + std::to_string(attempt) + " doors failed: " + e.what();
            continue;
        }
        const std::string why = valid_layout(layout, l, idx);
        if (why.empty()) {
            out.layout = std::move(layout);
            out.index_to_id = ids;
            out.seed_used = seed_a;
            out.attempt_used = attempt;
            return true;
        }
        last_diag =
            "attempt " + std::to_string(attempt) + " produced an invalid layout: " + why;
    }
    err = "generate: no valid layout in " + std::to_string(opts.attempts) + " attempt(s) [rooms " +
          std::to_string(ids.size()) + ", passages " + std::to_string(l.passages.size()) +
          ", base seed " + std::to_string(base_seed);
    if (opts.time_budget_ms) err += ", time budget " + std::to_string(*opts.time_budget_ms) + "ms";
    if (opts.iteration_budget) err += ", iteration budget " + std::to_string(*opts.iteration_budget);
    err += "]: " + last_diag +
           " (raise the budgets/attempts or loosen the graph; see docs/generate_v1.md)";
    return false;
}

bool write_layout_json(const LayoutResult& result, const Project& project,
                       const std::string& project_path, std::string& text_out, std::string& err) {
    if (!project.layout) {
        err = "layout: project has no layout tier (needs delve-project/1)";
        return false;
    }
    std::map<std::string, std::string> role_of;
    for (const auto& r : project.layout->rooms) role_of[r.id] = r.role;
    struct RoomEntry {
        std::string id, role, tmpl;
        bool corridor = false;
        std::vector<CellPt> grid;
        struct Door {
            std::string to;
            CellPt g0, g1;
        };
        std::vector<Door> doors;
    };
    std::vector<RoomEntry> rooms;
    for (const auto& room : result.layout.rooms) {
        if (room.room < 0 || static_cast<size_t>(room.room) >= result.index_to_id.size()) {
            err = "layout: result references unknown room index";
            return false;
        }
        RoomEntry e;
        e.id = result.index_to_id[static_cast<size_t>(room.room)];
        e.role = role_of[e.id];
        e.tmpl = room.room_template.name();
        e.corridor = room.is_corridor;
        for (const auto& p : room.outline.points())
            e.grid.push_back({p.x + room.position.x, p.y + room.position.y});
        if (contour_area2(e.grid) > 0) std::reverse(e.grid.begin(), e.grid.end());
        for (const auto& d : room.doors) {
            if (d.to_room < 0 || static_cast<size_t>(d.to_room) >= result.index_to_id.size()) {
                err = "layout: result references unknown door room index";
                return false;
            }
            const CellPt a{d.door_line.from.x + room.position.x,
                           d.door_line.from.y + room.position.y};
            const CellPt b{d.door_line.to.x + room.position.x, d.door_line.to.y + room.position.y};
            e.doors.push_back({result.index_to_id[static_cast<size_t>(d.to_room)], std::min(a, b),
                               std::max(a, b)});
        }
        std::sort(e.doors.begin(), e.doors.end(), [](const RoomEntry::Door& a, const RoomEntry::Door& b) {
            if (a.to != b.to) return a.to < b.to;
            if (a.g0 != b.g0) return a.g0 < b.g0;
            return a.g1 < b.g1;
        });
        rooms.push_back(std::move(e));
    }
    std::sort(rooms.begin(), rooms.end(),
              [](const RoomEntry& a, const RoomEntry& b) { return a.id < b.id; });

    nlohmann::ordered_json doc;
    doc["format"] = kLayoutFormat;
    doc["source"] = {{"project", project_path}, {"seed", result.seed_used}};
    nlohmann::ordered_json jrooms = nlohmann::ordered_json::array();
    for (const auto& r : rooms) {
        nlohmann::ordered_json jr;
        jr["id"] = r.id;
        jr["role"] = r.role;
        jr["template"] = r.tmpl;
        jr["corridor"] = r.corridor;
        nlohmann::ordered_json jgrid = nlohmann::ordered_json::array();
        for (const auto& [gx, gy] : r.grid) jgrid.push_back({gx, gy});
        jr["grid"] = std::move(jgrid);
        nlohmann::ordered_json jdoors = nlohmann::ordered_json::array();
        for (const auto& d : r.doors) {
            nlohmann::ordered_json jd;
            jd["to"] = d.to;
            jd["grid"] = {{d.g0.first, d.g0.second}, {d.g1.first, d.g1.second}};
            jdoors.push_back(std::move(jd));
        }
        jr["doors"] = std::move(jdoors);
        jrooms.push_back(std::move(jr));
    }
    doc["rooms"] = std::move(jrooms);
    text_out = doc.dump(1) + "\n";
    return true;
}

}  // namespace delve::layout
