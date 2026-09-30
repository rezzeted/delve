#pragma once

// Delve project v1: layout tier (docs/project_v1.md). D2, F1.
// Graph (rooms/passages), explicit templates, parametric ranges, and the
// cross-tier checks (R-G3, 5.4, catalog budget). No edgar types here; the F2
// catalog in delve_layout consumes these declarations.

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace delve {

using CellPt = std::pair<int, int>;  // (gx, gy), grid units

// Room/template level of the 4.2 hierarchy (field-wise overrides).
struct FillOverride {
    std::optional<double> h;
    std::optional<double> wall_t;
    std::optional<std::string> style, floor, ceil;
    bool empty() const { return !h && !wall_t && !style && !floor && !ceil; }
};

struct GraphRoom {
    std::string id, role;
    std::vector<std::string> tags;
    FillOverride fill;
};

struct Passage {
    std::string a, b;
    std::string door = "open";
};

struct TemplateDoors {
    bool manual = false;
    std::optional<int> length;           // simple override (nullopt = project)
    std::optional<int> corner_distance;  // simple override (nullopt = project)
    std::vector<std::pair<CellPt, CellPt>> segments;  // manual (template-local cells)
};

struct TemplateDecl {
    std::string name;
    std::vector<std::string> roles;  // adopter roles, >= 1
    std::vector<CellPt> contour;     // orthogonal simple polygon, any winding
    TemplateDoors doors;             // default: project simple rule
    bool transforms_set = false;
    std::vector<std::string> transforms;  // empty + set = identity only (port semantics)
    FillOverride fill;                 // template level of 4.2
};

struct IntRange {
    int lo = 1, hi = 1;
};

struct LayoutParams {
    int corridor_width = 2;
    IntRange corridor_length{3, 6};
    IntRange rect_w{4, 8}, rect_h{4, 8};
    bool rect_roles_set = false;
    std::vector<std::string> rect_roles;  // empty + unset = all non-corridor graph roles
    int door_length = 1;
    int door_corner_distance = 1;
    int min_room_distance = 0;
    int catalog_budget = 512;
    std::vector<GraphRoom> rooms;
    std::vector<Passage> passages;
    std::vector<TemplateDecl> templates;
};

inline constexpr const char* kProjectFormatV1 = "delve-project/1";

// Default transforms for templates that don't declare any (R-C4): rotations,
// no reflections. An explicitly empty list means identity only (port semantics).
inline const std::vector<std::string>& default_transforms() {
    static const std::vector<std::string> k = {"identity", "rot90", "rot180", "rot270"};
    return k;
}

inline std::vector<std::string> effective_transforms(const TemplateDecl& t) {
    return t.transforms_set ? t.transforms : default_transforms();
}

// Fixed role set (v1). "*" is not a room/template role (default level only).
bool is_concrete_role(const std::string& name);

// Name tables (implemented in project.cpp over style_code/door_code).
bool is_style_name(const std::string& name);
bool is_door_name(const std::string& name);

// Strict parse of the "layout" object (R-P2). False + err (path-prefixed,
// key/expectation/fact) on any violation, including contour geometry.
bool parse_layout(const nlohmann::json& j, const std::string& path, LayoutParams& out,
                  std::string& err);

// Strict parse of a room/template "fill" override object.
bool parse_fill_override(const nlohmann::json& j, const std::string& path, const std::string& where,
                         FillOverride& out, std::string& err);

// Graph helpers (R-G3): neighbors of a room id; false + err naming the first
// unreachable room id when the passage graph is disconnected.
std::vector<std::string> passage_neighbors(const LayoutParams& l, const std::string& id);
bool check_connected(const LayoutParams& l, std::string& bad_room);

// Parametric template counts (R-C2): corridors (0 when the graph has no
// corridor rooms) and rects (0 when narrowed away from every graph role).
int parametric_corridor_count(const LayoutParams& l);
int parametric_rect_count(const LayoutParams& l);
int catalog_size(const LayoutParams& l);  // parametric + explicit

// Roles of the graph lacking any template (parametric or explicit).
std::vector<std::string> roles_without_template(const LayoutParams& l);

// Signed area x2 of a cell contour (shoelace). edgar's PolygonGrid2D accepts
// area2 < 0 ("clockwise" by its own test); the F2 catalog reverses the rest.
long long contour_area2(const std::vector<CellPt>& c);

// Minimum bbox side of a template contour (cells). Used by the 5.4 corridor
// sanity check for explicit corridor-role templates.
int template_min_bbox_side(const TemplateDecl& t);

// Minimum endpoint-to-nearest-corner distance over manual door segments
// (cells, -1 when the template has no manual doors). Used by the 5.4 check.
int manual_door_min_corner(const TemplateDecl& t);

inline constexpr const char* kLayoutFormat = "delve-layout/0";

// Parsed F3 output (F4 input): live layouts serialize to this (delve_layout),
// tests and frozen flows read it back without edgar.
struct LayoutRoomData {
    struct Door {
        std::string to;
        CellPt g0, g1;  // world grid endpoints, lex-min first
    };
    std::string id, role, tmpl;
    bool corridor = false;
    std::vector<CellPt> grid;  // world coords, area2 < 0
    std::vector<Door> doors;   // by (to, g0, g1)
};

struct LayoutData {
    std::string source_project;
    int source_seed = 0;
    std::vector<LayoutRoomData> rooms;  // by id
};

// Parse a delve-layout/0 document. Rejects other formats (N7); sorts
// rooms/doors deterministically.
bool read_layout_json(const std::string& text, LayoutData& out, std::string& err);

}  // namespace delve
