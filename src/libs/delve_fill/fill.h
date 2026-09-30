#pragma once

// Delve fill (D1.3, F6): slot asset checks (R-A3) + level fill from IR v1.

#include <string>
#include <vector>

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
bool check_asset(const std::string& slot, const std::string& asset_path,
                 const std::vector<std::string>& import_roots,
                 std::vector<SlotDiag>& diags);

}  // namespace delve
