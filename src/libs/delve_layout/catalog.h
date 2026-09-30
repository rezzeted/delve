#pragma once

// Delve F2 catalog (docs/catalog_v1.md): project v1 declarations -> edgar
// templates + per-room descriptions. Deterministic order: parametric
// corridors, parametric rects, explicit templates (declaration order).

#include <map>
#include <string>
#include <vector>

#include "edgar/generator/grid2d/room_description_grid2d.hpp"
#include "edgar/generator/grid2d/room_template_grid2d.hpp"

#include "project.h"

namespace delve::layout {

struct CatalogEntry {
    std::string name;
    std::vector<std::string> roles;
    edgar::generator::grid2d::RoomTemplateGrid2D edgar;
    FillOverride fill;
    bool parametric = false;
};

struct CatalogStats {
    int templates = 0;
    int instances = 0;  // templates x transforms after symmetry merging
    int corridors = 0;
    int rects = 0;
    int explicit_count = 0;
};

struct Catalog {
    std::vector<CatalogEntry> entries;
    CatalogStats stats;
};

// False + err when the project has no layout tier, an explicit name collides
// with a parametric one, or an edgar constructor rejects a declaration.
bool build_catalog(const Project& project, Catalog& out, std::string& err);

// Per-graph-room descriptions (corridor flag + template copies). Empty sets
// are impossible after R-G3; one is reported as an internal error.
bool build_room_descriptions(
    const Project& project, const Catalog& catalog,
    std::map<std::string, edgar::generator::grid2d::RoomDescriptionGrid2D>& out, std::string& err);

}  // namespace delve::layout
