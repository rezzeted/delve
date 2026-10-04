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

// Rule-set version of the elements check (B3). Bump when the per-unit logic
// changes: cached verdicts with a different version are misses.
inline constexpr uint64_t kElementsCheckVersion = 1;

// Elements (connected face groups per unit): uniform @style; no coincident
// same-normal faces (double geometry; touching solids have opposite normals,
// embedded parts live on different planes, so both pass).
bool check_elements(const FillResult& fill, std::vector<CheckDiag>& diags);

// B3: check_elements over the F8 cache. Spans carry cacheKey only when fill
// ran with a UnitCache; key-less spans (and cache == nullptr) are checked
// live, exactly as check_elements. Cached verdicts are rigid-transform
// invariant (elements logic is), unit-id-relative, and versioned by
// kElementsCheckVersion; messages are re-prefixed with the current span id
// on replay, so the diag stream matches an uncached run.
bool check_elements_cached(const FillResult& fill, UnitCache* cache,
                           std::vector<CheckDiag>& diags);

// Unit-scoped elements check (F11-fast): same per-unit logic as
// check_elements, restricted to units whose id contains unit_substr; global
// checks are the caller's choice (skipped in --unit mode). Zero matching
// units is an error (probably a typo in the filter). matched_out, when set,
// receives the number of matched units.
bool check_units(const FillResult& fill, const std::string& unit_substr,
                 std::vector<CheckDiag>& diags, size_t* matched_out = nullptr);

// Facing dressing belongs to its room's side (5.2): every mesh point of a
// facing unit lies inside (or within 1e-4 of) the room's contour. Catches
// side inversions that zone/span checks cannot see (they are s/l-relative).
bool check_facing_bounds(const IrV2& ir, const Project& project, const FillResult& fill,
                         std::vector<CheckDiag>& diags);

bool check_level(const IrV2& ir, const Project& project, const FillResult& fill,
                 std::vector<CheckDiag>& diags);

// check_level with the elements check served through the F8 unit cache (B3):
// reused units replay their cached verdicts, only reran units are re-checked.
// Global checks (passage/spans/anchors/…) always run live — they are cheap.
bool check_level_cached(const IrV2& ir, const Project& project, const FillResult& fill,
                        UnitCache* cache, std::vector<CheckDiag>& diags);

}  // namespace delve
