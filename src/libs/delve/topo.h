#pragma once

// Delve topology view model: a pure projection of the level graph (the
// layout tier of the project) and the generated layout (delve-layout/0 data)
// into GUI-free view data — rooms as nodes (contours, centroids, roles),
// passages as edges (door type, label points), layout doors as plan segments.
// N4: no ImGui/sokol here; the DelveViewer Topo tab renders this, and the
// same projection is reusable by CLI/MCP. Ordering is deterministic (N6):
// nodes by id, edges by (a, b), doors by (room, to, g0, g1).

#include "layout.h"

namespace delve {

// Area centroid of a closed cell contour (shoelace formula, winding
// agnostic). Degenerate (zero-area) or empty contours fall back to the bbox
// midpoint with ok = false; real contours never degenerate (F2 rejects
// zero-area templates).
struct CellCentroid {
    bool ok = false;
    double x = 0.0;
    double z = 0.0;
};

CellCentroid contour_centroid(const std::vector<CellPt>& c);

// Axis-aligned bbox of a cell contour (inclusive). Empty contour gives
// (0, 0, 0, 0).
void grid_bbox(const std::vector<CellPt>& c, int& minx, int& maxx, int& miny, int& maxy);

// Door type of the passage between two room ids (either direction); "" when
// there is no such passage (layouts are built from the graph, so this
// should not happen for layout doors).
std::string passage_door(const LayoutParams& g, const std::string& a, const std::string& b);

// One room of the graph/layout union: graph metadata (role, tags) plus the
// placed geometry when the layout has the room.
struct TopoNode {
    std::string id;
    std::string role;  // "" for a layout-only ghost (should not happen)
    std::vector<std::string> tags;
    bool hasLayout = false;
    bool corridor = false;
    std::string tmpl;  // template used by the layout
    std::vector<CellPt> contour;  // world grid cells (empty when !hasLayout)
    double cx = 0.0, cz = 0.0;  // centroid, world grid cells
    int minx = 0, maxx = 0, miny = 0, maxy = 0;  // bbox, world grid cells
};

// A passage as a graph edge; the label point is the midpoint of the node
// centroids, present only when both endpoints are placed.
struct TopoEdge {
    std::string a, b;
    std::string door;
    bool labelOk = false;
    double lx = 0.0, lz = 0.0;
};

// A door segment of the plan (world grid cells), door type resolved through
// the passage graph by the (room, to) pair (R-G1: edgar sockets do not
// survive the layout).
struct TopoDoor {
    std::string room;
    std::string to;
    CellPt g0, g1;
    std::string door;
};

struct TopoModel {
    std::vector<TopoNode> nodes;    // by id
    std::vector<TopoEdge> edges;    // by (a, b)
    std::vector<TopoDoor> doors;    // by (room, to, g0, g1)
    std::vector<std::string> roles;  // distinct sorted graph roles (palette indices)
};

// Projects the graph and the layout into a TopoModel. The node set is the
// union of both: a graph room the layout did not place keeps hasLayout =
// false (the viewer can still draw the graph for an unplaced level), and a
// layout room missing from the graph is kept with an empty role.
TopoModel build_topo(const LayoutParams& graph, const LayoutData& layout);

}  // namespace delve
