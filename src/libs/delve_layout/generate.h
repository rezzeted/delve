#pragma once

// Delve F3 generation (docs/generate_v1.md): project + catalog -> edgar
// layouts with seed, budgets, cancel and attempts; layout/0 serialization.

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "edgar/generator/grid2d/graph_based_generator_grid2d.hpp"
#include "edgar/generator/grid2d/layout_grid2d.hpp"

#include "catalog.h"
#include "project.h"

namespace delve::layout {

struct GenerateOptions {
    int attempts = 1;  // >= 1; attempt k uses layout_seed + k
    std::optional<int> time_budget_ms;   // >= 0; early-stop wall clock
    std::optional<int> iteration_budget;  // >= 0; early-stop work units
};

struct LayoutResult {
    // room = graph index (index_to_id maps it to the stable graph id).
    edgar::generator::grid2d::LayoutGrid2D<int> layout;
    std::vector<std::string> index_to_id;
    int seed_used = 0;
    int attempt_used = 0;
    double time_ms = 0;  // successful (or last) attempt
    int iterations = 0;
};

class LayoutGenerator {
public:
    // Cooperative cancel (preview runs). Thread-safe; sticky until reset.
    // No-op when the run carries budgets (port limitation: cancel and
    // early-stop are mutually exclusive) or when nothing runs.
    void request_cancel();
    void reset_cancel();

    // One generate() at a time per instance. False + err on bad options,
    // cancel ("cancelled"), or no valid layout in N attempts (diagnostic
    // names rooms/passages/seed/budgets, F3).
    bool generate(const Project& project, const Catalog& catalog, const GenerateOptions& opts,
                  LayoutResult& out, std::string& err);

private:
    std::mutex mu_;
    edgar::generator::grid2d::GraphBasedGeneratorGrid2D<int>* active_ = nullptr;  // guarded
    bool cancelable_ = false;   // guarded
    bool sticky_cancel_ = false;  // guarded
};

// Serialize a result to delve-layout/0 (stable keys, N6). Rooms by id, doors
// by (to, g0, g1); contours area2 < 0; door segments lex-min first.
bool write_layout_json(const LayoutResult& result, const Project& project,
                       const std::string& project_path, std::string& text_out, std::string& err);

}  // namespace delve::layout
