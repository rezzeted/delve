#include "pch.h"

#include "ir_dump.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "edgar/geometry/grid_polygon_partitioning.hpp"
#include "edgar/io/dungeon_drawer.hpp"
#include "edgar/io/layout_grid_cells.hpp"

namespace delve::d0 {
namespace {

namespace fs = std::filesystem;
namespace grid2d = edgar::generator::grid2d;

std::string fmt_num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    return buf;
}

// Room palette for d0_view.pgg (warm beiges; corridors get gray).
const char* room_color(int room_id, bool corridor) {
    static constexpr const char* kPalette[] = {
        "(0.82, 0.71, 0.59)", "(0.76, 0.80, 0.66)", "(0.85, 0.78, 0.68)",
        "(0.70, 0.76, 0.80)", "(0.83, 0.69, 0.62)", "(0.74, 0.72, 0.78)",
    };
    if (corridor) return "(0.55, 0.55, 0.58)";
    return kPalette[room_id < 0 ? 0 : static_cast<size_t>(room_id) % (sizeof(kPalette) / sizeof(kPalette[0]))];
}

struct WorldRect {
    int room = 0;
    bool corridor = false;
    double x0 = 0, x1 = 0, z0 = 0, z1 = 0;
};

}  // namespace

long long contour_area2(const std::vector<std::pair<int, int>>& contour) {
    long long area2 = 0;
    for (size_t i = 0; i < contour.size(); ++i) {
        const auto [x0, y0] = contour[i];
        const auto [x1, y1] = contour[(i + 1) % contour.size()];
        area2 += static_cast<long long>(x0) * y1 - static_cast<long long>(x1) * y0;
    }
    return area2;
}

bool dump_frozen_ir(const grid2d::LayoutGrid2D<int>& layout, const DumpConfig& cfg,
                    const std::string& out_dir, std::string& err) {
    if (layout.rooms.empty()) {
        err = "layout has no rooms";
        return false;
    }
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    if (ec) {
        err = "cannot create " + out_dir + ": " + ec.message();
        return false;
    }

    struct RoomEntry {
        int id = 0;
        bool corridor = false;
        std::vector<std::pair<int, int>> grid;  // world coords (outline + position), CCW-normalized
        struct Door {
            int to = 0;
            std::pair<int, int> g0, g1;  // world grid endpoints
        };
        std::vector<Door> doors;
    };
    std::vector<RoomEntry> rooms;
    rooms.reserve(layout.rooms.size());
    for (const auto& room : layout.rooms) {
        RoomEntry e;
        e.id = room.room;
        e.corridor = room.is_corridor;
        for (const auto& p : room.outline.points()) e.grid.push_back({p.x + room.position.x, p.y + room.position.y});
        // §5.1: Delve normalizes the winding; PGG gets CCW seen from +Y (area2 < 0 in (x, z),
        // which under the identity mapping is area2 < 0 in (gx, gy) — what edgar already emits).
        if (contour_area2(e.grid) > 0) std::reverse(e.grid.begin(), e.grid.end());
        for (const auto& d : room.doors) {
            const int other = (d.from_room == room.room) ? d.to_room : d.from_room;
            e.doors.push_back({other, {d.door_line.from.x + room.position.x, d.door_line.from.y + room.position.y},
                               {d.door_line.to.x + room.position.x, d.door_line.to.y + room.position.y}});
        }
        std::sort(e.doors.begin(), e.doors.end(), [](const RoomEntry::Door& a, const RoomEntry::Door& b) {
            if (a.to != b.to) return a.to < b.to;
            if (a.g0 != b.g0) return a.g0 < b.g0;
            return a.g1 < b.g1;
        });
        rooms.push_back(std::move(e));
    }
    std::sort(rooms.begin(), rooms.end(), [](const RoomEntry& a, const RoomEntry& b) { return a.id < b.id; });

    // --- frozen_ir.json (N6: stable key order via ordered_json) ---
    nlohmann::ordered_json ir;
    ir["format"] = kIrFormat;
    ir["source"] = {{"map", cfg.map_name}, {"seed", cfg.seed}};
    ir["cell"] = cfg.cell;
    ir["mapping"] = "x = gx * cell; z = gy * cell";
    nlohmann::ordered_json jrooms = nlohmann::ordered_json::array();
    for (const auto& r : rooms) {
        nlohmann::ordered_json jr;
        jr["id"] = r.id;
        jr["corridor"] = r.corridor;
        nlohmann::ordered_json jgrid = nlohmann::ordered_json::array();
        nlohmann::ordered_json jcontour = nlohmann::ordered_json::array();
        for (const auto& [gx, gy] : r.grid) {
            jgrid.push_back({gx, gy});
            jcontour.push_back({grid_to_x(gx, cfg.cell), grid_to_z(gy, cfg.cell)});
        }
        jr["grid"] = std::move(jgrid);
        jr["contour"] = std::move(jcontour);
        nlohmann::ordered_json jdoors = nlohmann::ordered_json::array();
        for (const auto& d : r.doors) {
            nlohmann::ordered_json jd;
            jd["to"] = d.to;
            jd["grid"] = {{d.g0.first, d.g0.second}, {d.g1.first, d.g1.second}};
            jd["segment"] = {{grid_to_x(d.g0.first, cfg.cell), grid_to_z(d.g0.second, cfg.cell)},
                             {grid_to_x(d.g1.first, cfg.cell), grid_to_z(d.g1.second, cfg.cell)}};
            jdoors.push_back(std::move(jd));
        }
        jr["doors"] = std::move(jdoors);
        jrooms.push_back(std::move(jr));
    }
    ir["rooms"] = std::move(jrooms);
    {
        std::ofstream out(fs::path(out_dir) / "frozen_ir.json", std::ios::binary | std::ios::trunc);
        if (!out) {
            err = "cannot write frozen_ir.json";
            return false;
        }
        out << ir.dump(1) << "\n";
        out.close();
        if (!out) {
            err = "cannot write frozen_ir.json";
            return false;
        }
    }

    // --- rooms.points.json (pgg-points/1): interior lattice nodes, same predicate as DungeonDrawer ---
    struct Pt {
        int room = 0;
        bool corridor = false;
        int gx = 0, gy = 0;
    };
    std::vector<Pt> points;
    for (const auto& room : layout.rooms) {
        std::vector<edgar::geometry::Vector2Int> world;
        for (const auto& p : room.outline.points()) world.push_back(p + room.position);
        int min_x = world[0].x, min_y = world[0].y, max_x = world[0].x, max_y = world[0].y;
        for (const auto& p : world) {
            min_x = std::min(min_x, p.x);
            min_y = std::min(min_y, p.y);
            max_x = std::max(max_x, p.x);
            max_y = std::max(max_y, p.y);
        }
        for (int y = min_y; y <= max_y; ++y)
            for (int x = min_x; x <= max_x; ++x)
                if (edgar::io::point_in_polygon_xy({x, y}, world))
                    points.push_back({room.room, room.is_corridor, x, y});
    }
    std::sort(points.begin(), points.end(), [](const Pt& a, const Pt& b) {
        if (a.room != b.room) return a.room < b.room;
        if (a.gy != b.gy) return a.gy < b.gy;
        return a.gx < b.gx;
    });
    {
        nlohmann::ordered_json doc;
        doc["format"] = "pgg-points/1";
        nlohmann::ordered_json pos = nlohmann::ordered_json::array();
        nlohmann::ordered_json room_col = nlohmann::ordered_json::array();
        nlohmann::ordered_json corr_col = nlohmann::ordered_json::array();
        for (const auto& p : points) {
            pos.push_back({grid_to_x(p.gx, cfg.cell), 0.0, grid_to_z(p.gy, cfg.cell)});
            room_col.push_back(p.room);
            corr_col.push_back(p.corridor);
        }
        doc["positions"] = std::move(pos);
        doc["attrs"] = {{"room", {{"type", "int"}, {"values", std::move(room_col)}}},
                        {"corridor", {{"type", "bool"}, {"values", std::move(corr_col)}}}};
        std::ofstream out(fs::path(out_dir) / "rooms.points.json", std::ios::binary | std::ios::trunc);
        if (!out) {
            err = "cannot write rooms.points.json";
            return false;
        }
        out << doc.dump(1) << "\n";
        out.close();
        if (!out) {
            err = "cannot write rooms.points.json";
            return false;
        }
    }

    // --- d0_view.pgg (generated floor boxes from the rect partition) ---
    {
        std::string view;
        std::string render_err;
        if (!render_view_pgg(ir.dump(), view, render_err)) {
            err = render_err;
            return false;
        }
        std::ofstream out(fs::path(out_dir) / "d0_view.pgg", std::ios::binary | std::ios::trunc);
        if (!out) {
            err = "cannot write d0_view.pgg";
            return false;
        }
        out << view;
        out.close();
        if (!out) {
            err = "cannot write d0_view.pgg";
            return false;
        }
    }

    // --- reference.png (DungeonDrawer: the orientation etalon for §5.1) ---
    try {
        edgar::io::DungeonDrawer<int> drawer;
        drawer.draw_layout_and_save(layout, (fs::path(out_dir) / "reference.png").string());
    } catch (const std::exception& e) {
        err = std::string("reference.png: ") + e.what();
        return false;
    }
    return true;
}

bool render_view_pgg(const std::string& ir_json_text, std::string& pgg_text, std::string& err) {
    nlohmann::ordered_json ir;
    try {
        ir = nlohmann::ordered_json::parse(ir_json_text);
    } catch (const std::exception& e) {
        err = std::string("bad IR JSON: ") + e.what();
        return false;
    }
    if (ir.value("format", std::string{}) != kIrFormat) {
        err = std::string("unsupported IR format, expected ") + kIrFormat;
        return false;
    }
    const double cell = ir.value("cell", 0.0);
    if (!(cell > 0.0)) {
        err = "bad IR cell size";
        return false;
    }
    if (!ir.contains("rooms") || !ir["rooms"].is_array()) {
        err = "bad IR rooms";
        return false;
    }
    // Rect partition per room (pure grid math on the IR contour — no edgar layout needed,
    // but the partition itself is edgar's public util over the stored grid contour).
    std::vector<WorldRect> rects;
    try {
        for (const auto& jr : ir["rooms"]) {
            const int id = jr.value("id", 0);
            const bool corridor = jr.value("corridor", false);
            if (!jr.contains("grid") || !jr["grid"].is_array()) {
                err = "bad IR contour";
                return false;
            }
            std::vector<edgar::geometry::Vector2Int> grid;
            for (const auto& pt : jr["grid"]) {
                if (!pt.is_array() || pt.size() != 2 || !pt[0].is_number_integer() ||
                    !pt[1].is_number_integer()) {
                    err = "bad IR contour";
                    return false;
                }
                grid.push_back({pt[0].get<int>(), pt[1].get<int>()});
            }
            if (grid.size() < 3) {
                err = "bad IR contour";
                return false;
            }
            std::vector<edgar::geometry::RectangleGrid2D> parts;
            try {
                parts = edgar::geometry::partition_orthogonal_polygon_to_rectangles(
                    edgar::geometry::PolygonGrid2D(grid));
            } catch (const std::exception& e) {
                err = std::string("partition failed: ") + e.what();
                return false;
            }
            std::sort(parts.begin(), parts.end(), [](const auto& a, const auto& b) {
                if (a.a.y != b.a.y) return a.a.y < b.a.y;
                return a.a.x < b.a.x;
            });
            for (const auto& r : parts)
                rects.push_back({id, corridor, grid_to_x(r.a.x, cell), grid_to_x(r.b.x, cell),
                                 grid_to_z(r.a.y, cell), grid_to_z(r.b.y, cell)});
        }
    } catch (const std::exception& e) {
        err = std::string("bad IR rooms: ") + e.what();
        return false;
    }
    std::ostringstream pgg;
    pgg << "# Delve D0 frozen view — generated from frozen_ir.json, do not edit.\n"
        << "# Open in PggViewer (OrthoTop) and compare with reference.png (§5.1).\n";
    std::vector<std::string> names;
    for (size_t i = 0; i < rects.size(); ++i) {
        const WorldRect& r = rects[i];
        const std::string name = "r" + std::to_string(i);
        names.push_back(name);
        pgg << name << " = set(transform(box(size = (" << fmt_num(r.x1 - r.x0) << ", 0.2, "
            << fmt_num(r.z1 - r.z0) << "), res = 1), translate = (" << fmt_num((r.x0 + r.x1) * 0.5)
            << ", -0.1, " << fmt_num((r.z0 + r.z1) * 0.5) << ")), \"Cd\", " << room_color(r.room, r.corridor)
            << ", domain = points)\n";
    }
    pgg << "floors = merge(";
    for (size_t i = 0; i < names.size(); ++i) pgg << (i ? ", " : "") << names[i];
    pgg << ")\noutput floors\n";
    pgg_text = pgg.str();
    return true;
}

}  // namespace delve::d0
