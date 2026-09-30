#include "pch.h"

#include "catalog.h"

#include <algorithm>
#include <memory>
#include <set>

#include "edgar/generator/grid2d/configuration_spaces_generator.hpp"
#include "edgar/generator/grid2d/manual_door_mode_grid2d.hpp"
#include "edgar/generator/grid2d/simple_door_mode_grid2d.hpp"
#include "edgar/geometry/polygon_grid2d.hpp"

namespace delve::layout {
namespace {

namespace grid2d = edgar::generator::grid2d;

grid2d::RoomTemplateGrid2D make_simple(edgar::geometry::PolygonGrid2D outline, const std::string& name,
                                       int door_length, int corner_distance,
                                       std::vector<edgar::geometry::TransformationGrid2D> ts) {
    return grid2d::RoomTemplateGrid2D(
        std::move(outline),
        std::make_shared<grid2d::SimpleDoorModeGrid2D>(door_length, corner_distance), name,
        std::nullopt, std::move(ts));
}

edgar::geometry::TransformationGrid2D to_edgar(const std::string& name) {
    using T = edgar::geometry::TransformationGrid2D;
    if (name == "rot90") return T::Rotate90;
    if (name == "rot180") return T::Rotate180;
    if (name == "rot270") return T::Rotate270;
    if (name == "mirror_x") return T::MirrorX;
    if (name == "mirror_y") return T::MirrorY;
    if (name == "diag13") return T::Diagonal13;
    if (name == "diag24") return T::Diagonal24;
    return T::Identity;  // "identity" (names are validated at F1)
}

std::vector<edgar::geometry::TransformationGrid2D> rotations_edgar() {
    using T = edgar::geometry::TransformationGrid2D;
    return {T::Identity, T::Rotate90, T::Rotate180, T::Rotate270};
}

// edgar's PolygonGrid2D ctor takes area2 < 0 only; flip the rest, keeping the
// first point first (deterministic, manual door positions unaffected).
std::vector<CellPt> edgar_winding(const std::vector<CellPt>& c) {
    if (contour_area2(c) <= 0) return c;
    std::vector<CellPt> out{c.front()};
    for (size_t i = c.size(); i-- > 1;) out.push_back(c[i]);
    return out;
}

}  // namespace

bool build_catalog(const Project& project, Catalog& out, std::string& err) {
    if (!project.layout) {
        err = "catalog: project has no layout tier (needs delve-project/1)";
        return false;
    }
    const LayoutParams& l = *project.layout;
    try {
        Catalog c;
        std::set<std::string> names;
        if (parametric_corridor_count(l) > 0) {
            for (int len = l.corridor_length.lo; len <= l.corridor_length.hi; ++len) {
                const std::string name =
                    "corridor_" + std::to_string(l.corridor_width) + "x" + std::to_string(len);
                names.insert(name);
                c.entries.push_back({name,
                                     {"corridor"},
                                     make_simple(edgar::geometry::PolygonGrid2D::get_rectangle(
                                                     l.corridor_width, len),
                                                 name, l.door_length, l.door_corner_distance,
                                                 rotations_edgar()),
                                     FillOverride{},
                                     true});
            }
            c.stats.corridors = parametric_corridor_count(l);
        }
        if (parametric_rect_count(l) > 0) {
            std::vector<std::string> roles;
            if (l.rect_roles_set) {
                for (const auto& r : l.rect_roles) {
                    const bool in_graph = std::any_of(l.rooms.begin(), l.rooms.end(),
                                                      [&](const GraphRoom& g) {
                                                          return g.role == r;
                                                      });
                    if (in_graph) roles.push_back(r);
                }
            } else {
                std::set<std::string> seen;
                for (const auto& r : l.rooms)
                    if (r.role != "corridor" && seen.insert(r.role).second) roles.push_back(r.role);
            }
            for (int w = l.rect_w.lo; w <= l.rect_w.hi; ++w)
                for (int h = l.rect_h.lo; h <= l.rect_h.hi; ++h) {
                    const std::string name =
                        "rect_" + std::to_string(w) + "x" + std::to_string(h);
                    names.insert(name);
                    c.entries.push_back(
                        {name, roles,
                         make_simple(edgar::geometry::PolygonGrid2D::get_rectangle(w, h), name,
                                     l.door_length, l.door_corner_distance, rotations_edgar()),
                         FillOverride{}, true});
                }
            c.stats.rects = parametric_rect_count(l);
        }
        for (size_t i = 0; i < l.templates.size(); ++i) {
            const TemplateDecl& t = l.templates[i];
            if (!names.insert(t.name).second) {
                err = "layout.templates[" + std::to_string(i) + "]: name \"" + t.name +
                      "\" collides with a parametric template (rename the explicit one)";
                return false;
            }
            std::vector<edgar::geometry::Vector2Int> pts;
            for (const auto& [x, y] : edgar_winding(t.contour)) pts.push_back({x, y});
            edgar::geometry::PolygonGrid2D outline(std::move(pts));
            std::shared_ptr<grid2d::IDoorModeGrid2D> doors;
            if (t.doors.manual) {
                std::vector<grid2d::DoorGrid2D> segs;
                for (const auto& [a, b] : t.doors.segments)
                    segs.push_back({{a.first, a.second}, {b.first, b.second}, nullptr});
                doors = std::make_shared<grid2d::ManualDoorModeGrid2D>(std::move(segs));
            } else {
                doors = std::make_shared<grid2d::SimpleDoorModeGrid2D>(
                    t.doors.length.value_or(l.door_length),
                    t.doors.corner_distance.value_or(l.door_corner_distance));
            }
            std::vector<edgar::geometry::TransformationGrid2D> ts;
            for (const auto& name : effective_transforms(t)) ts.push_back(to_edgar(name));
            c.entries.push_back({t.name, t.roles,
                                 grid2d::RoomTemplateGrid2D(std::move(outline), std::move(doors),
                                                            t.name, std::nullopt, std::move(ts)),
                                 t.fill, false});
        }
        c.stats.explicit_count = static_cast<int>(l.templates.size());
        c.stats.templates = static_cast<int>(c.entries.size());
        grid2d::ConfigurationSpacesGenerator cs;
        for (const auto& e : c.entries)
            c.stats.instances += static_cast<int>(cs.get_room_template_instances(e.edgar).size());
        out = std::move(c);
        return true;
    } catch (const std::exception& e) {
        err = std::string("catalog: ") + e.what();
        return false;
    }
}

bool build_room_descriptions(
    const Project& project, const Catalog& catalog,
    std::map<std::string, grid2d::RoomDescriptionGrid2D>& out, std::string& err) {
    if (!project.layout) {
        err = "catalog: project has no layout tier (needs delve-project/1)";
        return false;
    }
    try {
        std::map<std::string, grid2d::RoomDescriptionGrid2D> desc;
        for (const auto& room : project.layout->rooms) {
            std::vector<grid2d::RoomTemplateGrid2D> ts;
            for (const auto& e : catalog.entries)
                if (std::find(e.roles.begin(), e.roles.end(), room.role) != e.roles.end())
                    ts.push_back(e.edgar);
            if (ts.empty()) {
                err = "catalog: internal: room \"" + room.id + "\" has no templates (R-G3 missed it)";
                return false;
            }
            // C# CorridorRoomDescription is always stage 2 (two-stage chain
            // decomposition attaches corridors after the stage-one layout).
            const bool corridor = room.role == "corridor";
            desc.emplace(room.id, grid2d::RoomDescriptionGrid2D(corridor, std::move(ts),
                                                               corridor ? 2 : 1));
        }
        out = std::move(desc);
        return true;
    } catch (const std::exception& e) {
        err = std::string("catalog: ") + e.what();
        return false;
    }
}

}  // namespace delve::layout
