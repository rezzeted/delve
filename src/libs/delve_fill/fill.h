#pragma once

// Delve fill (D1.3, F6): slot asset checks (R-A3) + level fill from IR v1.
// F8: optional unit-output cache (cache.h), keyed by slot + bindings + asset
// content (R-A7); local-frame hits are placed at assembly.

#include <map>
#include <set>
#include <string>
#include <vector>

#include "pgg/eval.h"
#include "cache.h"
#include "ir.h"
#include "project.h"

namespace delve {

// One R-A3 finding. code is "delve/slot" for contract violations or the PGG
// diagnostic code (E100...) for static-stage errors, quoted verbatim.
struct SlotDiag {
    std::string code;
    std::string message;
    bool warning = false;
};

// R-A3 (slots §4): parse + expand + typecheck the asset, then compare its
// interface against the slot contract: required params (names + types),
// mesh/anchors outputs, defaults on extra params, slot_version() == 1.
// import_roots resolve the asset's imports (project roots + delve asset dir);
// the asset's own directory is implicit. True when no errors (warnings ok).
// Unknown slot kind -> false + a single delve/slot error.
// declared_params (optional) collects the asset's params — the host binds
// optional inputs (room_fill `cuts`, decor `pit`) only when declared, and a
// declared geo param without a default is host-bound stream data (slots §1):
// a unit without data for it gets an empty geo of the declared kind.
struct DeclaredParam {
    std::string base;     // pgg type base ("geo", "int", "f32", ...)
    std::string geoKind;  // for base == "geo": "points" / "mesh"
    bool hasDefault = false;
};
bool check_asset(const std::string& slot, const std::string& asset_path,
                 const std::vector<std::string>& import_roots,
                 std::vector<SlotDiag>& diags,
                 std::map<std::string, DeclaredParam>* declared_params = nullptr);

// Contract lint on top of check_asset: runs the asset once with minimal
// synthetic inputs from the slot contract (seg/contour/... fixtures built in
// code) and checks the outputs against the assembly (merge) and F11 rules:
// mesh/anchors geo kinds, no groups/detail/instances, no vec2/vec4 columns,
// @style:int + @Cd:vec3 on mesh points (except door — neutral palette),
// int @kind on non-empty anchors. Findings use the "delve/lint" code.
// A failed synthetic run (inputs too poor for the asset) downgrades the lint
// to a warning — inconclusive, not an error. Slower than check_asset (one
// real PGG run); fill_level deliberately stays on the static check.
bool lint_asset(const std::string& slot, const std::string& asset_path,
                const std::vector<std::string>& import_roots,
                std::vector<SlotDiag>& diags);

// R-A5 last-resort root: the PGG product lib (thirdparty/pgg/resources/pgg)
// the delve repo vendors as a submodule, located by walking up from the delve
// assets dir. Empty when the layout is not the repo one. Callers append it
// after the delve assets dir so slot assets can import lib.* (N3, §9.2).
std::string find_pgg_lib_root(const std::string& delveAssets);

// F6: expand the IR into units (slots §1 ids), run each unit's asset in its
// local frame (R-A9) and assemble the world-frame level: meshes merged,
// anchors merged with a per-point @label "<unit_id>#<kind>" (kind =
// light|spawn|poi; blocker anchors kind=4 are stripped into
// FillResult::occupied instead). C4 two-phase: room_fill units expand and run
// first so floor decor expansion sees the occupied registry. Deterministic:
// units sorted, merge in order.
// opts.delve_assets is the delve asset library dir (codes/patterns + v1
// assets); project asset_roots come first (R-A5). Returns false + err
// (delve/slot or delve/run, F10 style) on any failure.
struct FillStats {
    size_t rooms = 0, bodies = 0, facings = 0, nodes = 0, doors = 0, lamps = 0;
    // F8 cache accounting (empty when FillOpts::cache is null): unit ids that
    // were reused from the cache vs recomputed.
    std::vector<std::string> reused, reran;
};

struct FillResult {
    pgg::GeoPtr mesh;
    pgg::GeoPtr anchors;
    FillStats stats;
    // Occupied volumes reported by room_fill blocker anchors (@kind=4,
    // stripped from the merged anchors; C4). World-frame XZ cylinders; decor
    // rules keep clear of them and register their own placements here.
    struct Occupied {
        double x = 0, z = 0, r = 0;
        std::string label;  // "<unit_id>#blocker" or the placed decor unit id
    };
    std::vector<Occupied> occupied;
    // Per-unit point spans in mesh/anchors (merge order; F11 attribution, F12).
    struct UnitSpan {
        std::string id, slot;
        size_t meshBegin = 0, meshEnd = 0;
        size_t anchorsBegin = 0, anchorsEnd = 0;
        // F8 unit key (UnitKey::h) when fill ran with a UnitCache, else 0.
        // delve_check uses it to cache per-unit elements verdicts (B3).
        uint64_t cacheKey = 0;
    };
    std::vector<UnitSpan> units;
};

struct FillOpts {
    std::string delve_assets;
    unsigned threads = 0;        // 0 = PGG default (hardware)
    UnitCache* cache = nullptr;  // F8: caller-owned unit cache (null = off)
};

bool fill_level(const IrV2& ir, const Project& project, const FillOpts& opts,
                FillResult& out, std::string& err);

}  // namespace delve
