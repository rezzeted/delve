#pragma once

// Delve export (D4, F7): the filled level as reviewable artifacts — the shared
// mesh as OBJ (@Cd goes out as vertex colors, @N as vn), anchors as a
// machine-readable pgg-points/1 file, per-unit attribution as delve-units/1
// and the IR next to them (delve-ir/3). Formats: docs/cli_v1.md.

#include <string>
#include <vector>

#include "fill.h"
#include "ir.h"

namespace delve {

inline constexpr const char* kUnitsFormat = "delve-units/1";

struct ExportOpts {
    std::string dir;             // output directory, created when missing
    std::string name;            // artifact base name (the caller derives it)
    bool split_groups = false;   // also write one OBJ per unit (faces groups)
};

struct ExportResult {
    std::vector<std::string> written;  // artifact paths in write order
};

// Per-unit point/anchor spans of the fill as delve-units/1 JSON (stable key
// order, N6) — the machine-readable "groups" of the merged mesh (the mesh
// itself carries no groups by design, see fill.cpp mergeGeos).
bool write_units_json(const FillResult& fill, const std::string& path, std::string& err);

// Writes <dir>/<name>.obj, .anchors.json, .units.json, .ir.json and, with
// split_groups, one <dir>/<name>.<unit>.obj per unit. IO failures report
// "delve/export [<artifact>]: ...".
bool export_level(const IrV2& ir, const FillResult& fill, const ExportOpts& opts,
                  ExportResult& out, std::string& err);

}  // namespace delve
