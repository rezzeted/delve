#pragma once

// Delve geometric checks (D1.4, F11): numbers, no pictures. Each check takes
// the IR, the project and the F6 result and appends findings; false = failed.

#include <string>
#include <vector>

#include "fill.h"
#include "ir.h"
#include "project.h"

namespace delve {

struct CheckDiag {
    std::string check;    // passage | opening_voids | transitions | anchors | spans | elements |
                          // facing_bounds
    std::string message;  // F11/<check>: detail with numbers
};

// Corridor clear widths >= min_passage, door clear widths >= min_opening.
bool check_passage(const IrV2& ir, const Project& project, std::vector<CheckDiag>& diags);

// No styled faces inside door opening voids (body/facing cut mismatch and
// decor blocking share this signal: door parts are style 0, walls are not).
bool check_opening_voids(const IrV2& ir, const FillResult& fill, std::vector<CheckDiag>& diags);

// Zone elements: none straddles a zone boundary, paint matches the territory
// (butt/chase from slots §3, last covering piece wins, 1e-3 tolerance).
bool check_transitions(const IrV2& ir, const Project& project, const FillResult& fill,
                       std::vector<CheckDiag>& diags);

// Light anchors are not inside wall bodies or pillars.
bool check_anchors(const IrV2& ir, const Project& project, const FillResult& fill,
                   std::vector<CheckDiag>& diags);

// Analytic spans: colinear bodies disjoint, crossings and T-junctions sit on
// nodes, facings per room disjoint, pillars disjoint, facings clear of
// pillar interiors (touching is fine).
bool check_spans(const IrV2& ir, const Project& project, std::vector<CheckDiag>& diags);

// Elements (connected face groups per unit): uniform @style; no coincident
// same-normal faces (double geometry; touching solids have opposite normals,
// embedded parts live on different planes, so both pass).
bool check_elements(const FillResult& fill, std::vector<CheckDiag>& diags);

// Facing dressing belongs to its room's side (5.2): every mesh point of a
// facing unit lies inside (or within 1e-4 of) the room's contour. Catches
// side inversions that zone/span checks cannot see (they are s/l-relative).
bool check_facing_bounds(const IrV2& ir, const Project& project, const FillResult& fill,
                         std::vector<CheckDiag>& diags);

bool check_level(const IrV2& ir, const Project& project, const FillResult& fill,
                 std::vector<CheckDiag>& diags);

}  // namespace delve
