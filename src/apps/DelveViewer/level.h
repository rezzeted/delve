#pragma once

// DelveViewer data layer (F9): owns the project -> layout|frozen -> IR -> fill
// pipeline state so the UI reads plain structs. The UnitCache (F8) outlives
// individual fills; refill/relayout reuse it and expose the reuse stats.

#include <string>

#include "fill.h"
#include "ir.h"
#include "layout.h"
#include "project.h"

struct Level {
    delve::Project project;
    delve::LayoutData layoutData;  // meaningful when ir.from_layout
    delve::IrV2 ir;
    delve::FillResult fill;
    delve::UnitCache unitCache;  // F8: position-independent unit outputs

    std::string projectPath;
    std::string irPath;       // --ir source; empty = layout generated from the project
    std::string delveAssets;  // resolved assets dir (FillOpts::delve_assets)
    double layoutMs = 0.0;    // last successful generate (0 in --ir mode)
    double fillMs = 0.0;      // last fill_level
    bool loaded = false;

    // Full pipeline: load the project, take the IR from --ir or generate a
    // layout (attempts=4) and build it, then fill with the live cache.
    bool load(const std::string& project, const std::string& ir, const std::string& assets,
              std::string& err);
    // Re-read the project file, rebuild the IR from the stored layout (or
    // re-read the frozen file) and refill through the same cache.
    bool refill(std::string& err);
    // Regenerate the layout from the same project, rebuild the IR, refill.
    bool relayout(std::string& err);

  private:
    bool buildIrFromGenerate(std::string& err);
    bool readFrozenIr(const std::string& path, std::string& err);
    bool runFill(std::string& err);
};

// Locate the delve assets library (the dir holding codes.pgg): ./assets first,
// then walk up from the executable's directory, then from the project file's
// directory. Empty when not found.
std::string resolve_delve_assets(const std::string& argv0, const std::string& projectPath);

// Default dir of the project Browse dialog: <root>/projects next to the
// located assets library. Empty when no assets root matched.
std::string resolve_delve_projects(const std::string& argv0);
