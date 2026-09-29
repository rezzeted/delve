#pragma once

// Delve D0: one-shot frozen-IR dump from an edgar layout (requirements §11 D0).
// Scaffold: replaced by the F4 IR builder at D1. The delve-ir/0 schema here is
// minimal (rooms with meter contours + doors + the axis mapping under test)
// and versioned so D1 can migrate or reject it (N7).

#include <string>
#include <utility>
#include <vector>

#include "edgar/generator/grid2d/layout_grid2d.hpp"

namespace delve::d0 {

inline constexpr const char* kIrFormat = "delve-ir/0";

struct DumpConfig {
    std::string map_name = "tutorial_corridors.yml";
    int seed = 1;
    double cell = 2.0;  // meters per grid unit (fill-tier param, §4.1)
};

// Axis mapping under test (§5.1): world (x, z) from grid (gx, gy).
// D0 candidate, locked by the D0 test and the PggViewer top-view check:
//   x = gx * cell, z = gy * cell.
inline double grid_to_x(int gx, double cell) { return static_cast<double>(gx) * cell; }
inline double grid_to_z(int gy, double cell) { return static_cast<double>(gy) * cell; }

// Signed area * 2 of a grid contour (shoelace). edgar outlines are clockwise
// in the math view (x right, y up), i.e. area2 < 0 — which under the identity
// mapping is exactly the CCW-seen-from-+Y that PGG plans require (§5.1).
long long contour_area2(const std::vector<std::pair<int, int>>& contour);

// Writes frozen_ir.json, rooms.points.json (pgg-points/1), d0_view.pgg and
// reference.png (DungeonDrawer) into out_dir. False + err on failure.
bool dump_frozen_ir(const edgar::generator::grid2d::LayoutGrid2D<int>& layout, const DumpConfig& cfg,
                    const std::string& out_dir, std::string& err);

// Pure regeneration of d0_view.pgg from frozen_ir.json text (no edgar) —
// the platform-stable half of the D0 test. False + err on bad input.
bool render_view_pgg(const std::string& ir_json_text, std::string& pgg_text, std::string& err);

}  // namespace delve::d0
