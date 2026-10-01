#pragma once

// Delve IR v3 (F4, docs/ir_v2.md): walls, nodes, doors, room developments and
// transitions. Two input paths into one core (D2.3b):
//   - build_ir_v2: frozen IR (delve-ir/0) + a fill project; v1 restrictions
//     (rects, 1-cell doors, dtype open, uniform project wall_t) hold.
//   - build_ir_from_layout: delve-layout/0 + a delve-project/1 (graph roles,
//     dtype from passage edges, multi-cell doors, figured orthogonal rooms,
//     per-room wall_t via resolve_room_fill, 5.2 mismatch is an F4 error).
// v2 = v1 geometry with string room ids (frozen decimal form, D2: graph id).
// v3 (D3, F8): position-independent wall/node ids (owner room + contour
// position), so a re-layout that moves rooms keeps their unit ids and cache
// keys; delve-ir/2 files stay readable (ids are opaque to the reader).

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "project.h"

namespace delve {

inline constexpr const char* kIrFormat = "delve-ir/3";
inline constexpr const char* kIrFormatV2 = "delve-ir/2";  // legacy, readable

using GridPt = std::pair<int, int>;          // (gx, gy), grid units
using WorldPt = std::pair<double, double>;   // (x, z), meters

struct IrRoom {
    std::string id;  // frozen decimal form (D2: graph id)
    bool corridor = false;
    std::string role;  // corridor | hall (v1 mapping)
    std::vector<GridPt> grid;  // CCW-normalized contour (area2 < 0), world grid coords
    double h = 0;
    std::string style, floor_style, ceil_style;
    // F12: resolution chains of h, style, floor, ceil, wall_t (§4.2; wall_t
    // lives only here and in walls/nodes). Empty for read /2 files.
    std::map<std::string, ProvChain> prov;
};

// One wall body unit (§5.2): a contour-edge atom split at every T-vertex.
// Axis endpoints are lex-min first; rooms are left/right of g0 -> g1.
struct IrWall {
    std::string id;  // wall:<owner_room>:<edge>[.<k>]; .k iff the owner's contour edge holds >1 atom
    bool outer = false;
    std::string owner;  // shared: min room id; outer: its room
    std::string room_left, room_right;  // "" = void
    GridPt g0, g1;
    double thick = 0;  // owner's wall_t, meters
    double t_end0 = 0, t_end1 = 0;  // node (pillar) thickness at g0 / g1 end
    double h_left = 0, h_right = 0;  // room heights; void side repeats owner h
    std::vector<std::string> doors;  // door ids cutting this wall, sorted
};

// One run of a transition zone on a unit's local axis (facing/node-face
// `zones` param, slots §3): t = s - s0_zone, t(l) = t_at_l0 + l (flip 0)
// or t_at_l0 - l (flip 1). Facings always run with +s (flip 0); node-face
// +x = right of the outward normal, flip covers the +s parity (slots §2.4).
struct ZonePiece {
    int zone = -1;
    int pattern = 0;  // 0 butt | 1 chase
    int seed = 0;     // zone rng, derived from the zone id
    double t_at_l0 = 0;
    int flip = 0;  // v1: always 0 (local axes run with +s)
    double width = 0, module = 0;  // meters
    double l0 = 0, l1 = 0;  // local-axis run, l0 < l1
    int style_a = 0, style_b = 0;  // style codes of the A/B sides (zone_style picks)
};

// One side facing of a wall (§5.2, slots §2.3). seg runs in the room's
// contour-walk direction on the body face plane (already offset by thick/2).
struct IrFacing {
    std::string id;  // fac:<room>:<edge>[.<k>]; .k iff the contour edge holds >1 atom
    std::string wall;
    std::string room;
    std::string style;  // 4.2 side resolution
    WorldPt from, to;  // seg ends, meters (y = 0 plane)
    WorldPt n;  // outward unit normal (xz)
    double h = 0;
    struct Cut {
        WorldPt a, b;  // full door segment (meters, y = 0 plane)
        double h = 0;  // opening height (door_h)
    };
    std::vector<Cut> cuts;  // walk order
    std::vector<ZonePiece> zones;  // ascending l
    double s0 = 0, s1 = 0;  // development interval, meters
    // F12: style chain = the room's style chain + fired side-rule steps.
    std::map<std::string, ProvChain> prov;
};

// One open pillar face (slots §2.4), node-local coords (origin = pillar
// center at base, axes world-aligned).
struct IrNodeFace {
    WorldPt center;
    WorldPt n;  // outward unit normal
    std::string room;  // room looked into ("" = void)
    double h = 0;  // looked-into room height (void: owner h)
    std::string style;  // looked-into side style (void: owner style)
    std::vector<ZonePiece> zones;  // ascending l (l = 0 at face center)
    // F12: style chain (copy of the looked-into room's / flank facing's).
    std::map<std::string, ProvChain> prov;
};

struct IrNode {
    std::string id;  // node:<owner_room>:<v>; v = vertex index in the owner's atomized contour
    std::string owner;  // min adjacent room id
    GridPt at;
    double thick = 0;
    double h_pillar = 0;  // max adjacent room h
    std::vector<IrNodeFace> faces;  // open faces only, sorted by normal
};

struct IrDoor {
    std::string id;  // door:<a>-<b>, a < b
    std::string room_a, room_b;
    std::string wall;
    GridPt g0, g1;  // full segment (1+ cells), lex-min first
    WorldPt from, to;  // CLEAR opening ends (frame already out), lex-min first
    double clear = 0;  // clear width, meters (door_len * cell - 2 * frame)
    double h = 0, frame = 0, thick = 0;
    int dtype = 1;  // frozen path: always open; layout path: passage door code
    // F12: dtype (passage | default), h/frame (project), thick (owner wall_t).
    std::map<std::string, ProvChain> prov;
};

struct IrTransition {
    int id = -1;  // stable: order by (room, s0)
    std::string room;  // development owner
    std::string style_a, style_b;  // incoming / outgoing (walk direction)
    int pattern = 0;
    double width = 0;  // configured width, meters
    std::string place;  // corner | wall
    double s0 = 0, s1 = 0;  // final (possibly shortened) zone on the development
    int seed = 0;  // zone rng, derived from the id
    bool shortened = false;
    // F12: pattern, width, place (one project step each).
    std::map<std::string, ProvChain> prov;
};

struct IrV2 {
    // Provenance: frozen path fills frozen_path; layout path sets from_layout
    // and layout_project/layout_seed (from delve-layout/0 source).
    std::string frozen_path, project_path;
    bool from_layout = false;
    std::string layout_project;
    int layout_seed = 0;
    std::vector<IrRoom> rooms;  // by id
    std::vector<IrWall> walls;  // by id
    std::vector<IrFacing> facings;  // by id
    std::vector<IrNode> nodes;  // by id
    std::vector<IrDoor> doors;  // by id
    std::vector<IrTransition> transitions;  // by id
    std::vector<std::string> warnings;  // shortenings, in deterministic order
    std::map<std::string, double> corridor_clear;  // corridor id -> min clear width, meters
};

// Build from frozen IR JSON text (delve-ir/0) + a loaded project. False + err
// on structural problems (unpaired door, door crossing a T, non-rect room,
// multi-cell door, bad 5.4 geometry); minima enforcement is F11, not F4.
bool build_ir_v2(const std::string& frozen_json, const std::string& frozen_path,
                 const Project& project, const std::string& project_path, IrV2& out,
                 std::string& err);

// D2.3b: build from a generated layout (delve-layout/0) + a delve-project/1.
// Roles/ids come from the graph, dtype from passage edges, per-room wall_t
// from resolve_room_fill (a shared wall whose sides resolve different
// thicknesses is an F4 error naming both rooms, 5.2). Figured orthogonal
// contours and multi-cell doors are accepted. False + err on any mismatch
// between the layout and the graph (unknown/missing room, role mismatch,
// door without a passage, unpaired door, self-intersecting contour).
bool build_ir_from_layout(const LayoutData& layout, const Project& project,
                          const std::string& project_path, IrV2& out, std::string& err);

// F5 artifact: stable-key JSON (N6). read rejects other formats (N7).
bool write_ir_v2_json(const IrV2& ir, std::string& text_out, std::string& err);
bool read_ir_v2_json(const std::string& text, IrV2& out, std::string& err);

// Unit/zone seeds (slots §1, §5.6): FNV-1a over "fill_seed/unit_id" resp.
// "transition/<id>", masked to 31 bits.
int unit_seed(int fill_seed, const std::string& unit_id);
int zone_seed(int zone_id);

}  // namespace delve
