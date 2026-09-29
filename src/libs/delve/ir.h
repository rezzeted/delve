#pragma once

// Delve IR v1 (F4, docs/ir_v1.md): walls, nodes, doors, room developments and
// transitions built from a frozen IR (delve-ir/0) + a fill project. No edgar.

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "project.h"

namespace delve {

inline constexpr const char* kIrFormat = "delve-ir/1";

using GridPt = std::pair<int, int>;          // (gx, gy), grid units
using WorldPt = std::pair<double, double>;   // (x, z), meters

struct IrRoom {
    int id = 0;  // frozen id; the v1 stable id is its decimal form (D2: graph id)
    bool corridor = false;
    std::string role;  // corridor | hall (v1 mapping)
    std::vector<GridPt> grid;  // CCW-normalized contour (area2 < 0), world grid coords
    double h = 0;
    std::string style, floor_style, ceil_style;
};

// One wall body unit (§5.2): a contour-edge atom split at every T-vertex.
// Axis endpoints are lex-min first; rooms are left/right of g0 -> g1.
struct IrWall {
    std::string id;  // wall:<x0>,<y0>-<x1>,<y1>
    bool outer = false;
    int owner = -1;  // shared: min room id; outer: its room
    int room_left = -1, room_right = -1;  // -1 = void
    GridPt g0, g1;
    double thick = 0;  // owner's wall_t, meters
    double h_left = 0, h_right = 0;  // room heights; void side repeats owner h
    std::vector<std::string> doors;  // door ids cutting this wall, sorted
};

// One run of a transition zone on a unit's local axis (facing/node-face
// `zones` param, slots §3): t = s - s0_zone, t(l) = t_at_l0 + l (flip=0, v1).
struct ZonePiece {
    int zone = -1;
    int pattern = 0;  // 0 butt | 1 chase
    int seed = 0;     // zone rng, derived from the zone id
    double t_at_l0 = 0;
    int flip = 0;  // v1: always 0 (local axes run with +s)
    double width = 0, module = 0;  // meters
    double l0 = 0, l1 = 0;  // local-axis run, l0 < l1
};

// One side facing of a wall (§5.2, slots §2.3). seg runs in the room's
// contour-walk direction on the body face plane (already offset by thick/2).
struct IrFacing {
    std::string id;  // fac:<room>:<edge>[.<k>]; .k iff the contour edge holds >1 atom
    std::string wall;
    int room = -1;
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
};

// One open pillar face (slots §2.4), node-local coords (origin = pillar
// center at base, axes world-aligned).
struct IrNodeFace {
    WorldPt center;
    WorldPt n;  // outward unit normal
    int room = -1;  // room looked into (-1 = void)
    double h = 0;  // looked-into room height (void: owner h)
    std::string style;  // looked-into side style (void: owner style)
    std::vector<ZonePiece> zones;  // ascending l (l = 0 at face center)
};

struct IrNode {
    std::string id;  // node:<gx>,<gy>
    int owner = -1;  // min adjacent room id
    GridPt at;
    double thick = 0;
    double h_pillar = 0;  // max adjacent room h
    std::vector<IrNodeFace> faces;  // open faces only, sorted by normal
};

struct IrDoor {
    std::string id;  // door:<a>-<b>, a < b
    int room_a = -1, room_b = -1;
    std::string wall;
    GridPt g0, g1;  // full 1-cell segment, lex-min first
    WorldPt from, to;  // CLEAR opening ends (frame already out), lex-min first
    double clear = 0;  // clear width, meters (door_len * cell - 2 * frame)
    double h = 0, frame = 0, thick = 0;
    int dtype = 1;  // v1: always open (frozen IR carries no socket ids)
};

struct IrTransition {
    int id = -1;  // stable: order by (room, s0)
    int room = -1;  // development owner
    std::string style_a, style_b;  // incoming / outgoing (walk direction)
    int pattern = 0;
    double width = 0;  // configured width, meters
    std::string place;  // corner | wall
    double s0 = 0, s1 = 0;  // final (possibly shortened) zone on the development
    int seed = 0;  // zone rng, derived from the id
    bool shortened = false;
};

struct IrV1 {
    std::string frozen_path, project_path;
    std::vector<IrRoom> rooms;  // by id
    std::vector<IrWall> walls;  // by id
    std::vector<IrFacing> facings;  // by id
    std::vector<IrNode> nodes;  // by id
    std::vector<IrDoor> doors;  // by id
    std::vector<IrTransition> transitions;  // by id
    std::vector<std::string> warnings;  // shortenings, in deterministic order
    std::map<int, double> corridor_clear;  // corridor id -> min clear width, meters
};

// Build from frozen IR JSON text (delve-ir/0) + a loaded project. False + err
// on structural problems (unpaired door, door crossing a T, non-rect room,
// multi-cell door, bad 5.4 geometry); minima enforcement is F11, not F4.
bool build_ir_v1(const std::string& frozen_json, const std::string& frozen_path,
                 const Project& project, const std::string& project_path, IrV1& out,
                 std::string& err);

// F5 artifact: stable-key JSON (N6). read rejects other formats (N7).
bool write_ir_v1_json(const IrV1& ir, std::string& text_out, std::string& err);
bool read_ir_v1_json(const std::string& text, IrV1& out, std::string& err);

// Unit/zone seeds (slots §1, §5.6): FNV-1a over "fill_seed/unit_id" resp.
// "transition/<id>", masked to 31 bits.
int unit_seed(int fill_seed, const std::string& unit_id);
int zone_seed(int zone_id);

}  // namespace delve
