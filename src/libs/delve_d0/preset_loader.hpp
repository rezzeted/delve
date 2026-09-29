// D0 SCAFFOLD — vendored verbatim from level-synth (src/apps/main/preset_loader.hpp).
// One-shot preset loading for the frozen-IR dump; replaced by the F2 catalog at D2.
// Do not extend: keep the diff against upstream empty.
#pragma once

#include "edgar/edgar.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace edgar::generator::grid2d {

struct PresetRoomSet {
    std::string name;
    struct RoomEntry {
        std::string name;
        std::vector<edgar::geometry::Vector2Int> shape;
        std::string door_mode;
        int door_length = 1;
        int corner_distance = 1;
        std::vector<std::pair<edgar::geometry::Vector2Int, edgar::geometry::Vector2Int>> specific_doors;
    };
    std::vector<RoomEntry> rooms;
    int default_door_length = 1;
    int default_corner_distance = 1;
    bool has_defaults = false;
};

struct PresetMap {
    std::string filename;
    std::string display_name;
    int room_from = 0;
    int room_to = 0;
    struct Passage {
        int a, b;
    };
    std::vector<Passage> passages;
    struct RoomOverride {
        std::vector<int> room_ids;
        std::vector<std::string> room_description_names;
    };
    std::vector<RoomOverride> room_overrides;
    struct DefaultRoomShapes {
        std::string set_name;
        std::string room_description_name;
        std::vector<int> scale;
    };
    std::vector<DefaultRoomShapes> default_room_shapes;
    PresetRoomSet custom_descriptions;
    bool has_custom_descriptions = false;
    bool corridors_enabled = false;
    std::vector<int> corridor_offsets;
    std::vector<DefaultRoomShapes> corridor_shapes;
};

struct PresetCatalog {
    std::vector<PresetMap> maps;
    std::vector<PresetRoomSet> room_sets;
    std::string base_path;
};

/// Result of loading a catalog; `error` empty on success.
struct PresetCatalogLoadResult {
    PresetCatalog catalog;
    std::string error;
};

/// Load all `Rooms/*.yml` and `Maps/*.yml` under `base_path`.
PresetCatalogLoadResult load_preset_catalog_with_status(const std::string& base_path);

/// Load room sets from base next to `Maps/`, only the map file `map_yml_path` (must exist).
/// `map_yml_path` should live under `.../Maps/name.yml`; base is derived as parent of `Maps`.
PresetCatalogLoadResult load_preset_catalog_from_map_file(const std::string& map_yml_path);

/// Legacy: on failure returns empty maps (prefer `load_preset_catalog_with_status`).
PresetCatalog load_preset_catalog(const std::string& base_path);

LevelDescriptionGrid2D<int> build_level_from_preset(
    const PresetMap& map, const std::vector<PresetRoomSet>& room_sets);

LevelDescriptionGrid2D<int> build_level_from_preset(const PresetMap& map, const PresetCatalog& catalog);

} // namespace edgar::generator::grid2d
