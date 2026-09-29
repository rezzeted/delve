// D0 SCAFFOLD — vendored verbatim from level-synth (src/apps/main/preset_loader.cpp).
// One-shot preset loading for the frozen-IR dump; replaced by the F2 catalog at D2.
// Do not extend: keep the diff against upstream empty.
#include "preset_loader.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

namespace edgar::generator::grid2d {

namespace {

/// Normalize YAML map key under `rooms:` to a comma-separated id string for the legacy parser below.
/// Matches original C# / YamlDotNet: scalar string, integer, or flow sequence like `[8]` / `[0,1]`.
std::string room_override_key_to_ids_string(const YAML::Node& key) {
    if (!key || !key.IsDefined()) {
        throw YAML::Exception(YAML::Mark::null_mark(), "rooms: entry has undefined key");
    }
    if (key.IsScalar()) {
        try {
            return key.as<std::string>();
        } catch (const YAML::Exception&) {
            try {
                return std::to_string(key.as<int>());
            } catch (const YAML::Exception&) {
                return std::to_string(static_cast<long long>(key.as<long long>()));
            }
        }
    }
    if (key.IsSequence()) {
        if (key.size() == 0) {
            throw YAML::Exception(key.Mark(), "rooms: empty sequence key");
        }
        std::string out;
        for (std::size_t i = 0; i < key.size(); ++i) {
            if (i > 0) {
                out.push_back(',');
            }
            const YAML::Node& el = key[i];
            if (!el.IsScalar()) {
                throw YAML::Exception(el.Mark(), "rooms: sequence key entry must be scalar");
            }
            try {
                out += el.as<std::string>();
            } catch (const YAML::Exception&) {
                try {
                    out += std::to_string(el.as<int>());
                } catch (const YAML::Exception&) {
                    out += std::to_string(static_cast<long long>(el.as<long long>()));
                }
            }
        }
        return out;
    }
    throw YAML::Exception(key.Mark(), "rooms: unsupported key type (use string, int, or [id,...])");
}

PresetRoomSet::RoomEntry parse_room_entry(const std::string& name, const YAML::Node& node, int default_door_length,
                                          int default_corner_distance) {
    PresetRoomSet::RoomEntry entry;
    entry.name = name;

    const auto& shape_node = node["shape"];
    if (shape_node && shape_node.IsSequence()) {
        for (const auto& pt : shape_node) {
            entry.shape.push_back(edgar::geometry::Vector2Int{pt[0].as<int>(), pt[1].as<int>()});
        }
    }

    const auto& dm = node["doorMode"];
    if (dm) {
        if (dm["doorLength"]) {
            entry.door_length = dm["doorLength"].as<int>();
        } else {
            entry.door_length = default_door_length;
        }
        if (dm["cornerDistance"]) {
            entry.corner_distance = dm["cornerDistance"].as<int>();
        } else {
            entry.corner_distance = default_corner_distance;
        }
        entry.door_mode = "OverlapMode";
        if (dm["doorPositions"]) {
            entry.door_mode = "SpecificPositionsMode";
            for (const auto& dp : dm["doorPositions"]) {
                entry.specific_doors.push_back({
                    edgar::geometry::Vector2Int{dp[0][0].as<int>(), dp[0][1].as<int>()},
                    edgar::geometry::Vector2Int{dp[1][0].as<int>(), dp[1][1].as<int>()},
                });
            }
        }
    } else {
        entry.door_length = default_door_length;
        entry.corner_distance = default_corner_distance;
        entry.door_mode = "OverlapMode";
    }

    return entry;
}

PresetRoomSet load_room_set(const std::string& path) {
    PresetRoomSet set;
    std::ifstream f(path);
    if (!f.is_open()) {
        return set;
    }

    YAML::Node root;
    try {
        root = YAML::Load(f);
    } catch (const std::exception&) {
        return set;
    }
    set.name = root["name"] ? root["name"].as<std::string>() : "";

    int def_dl = 1, def_cd = 1;
    if (const auto& def = root["default"]) {
        set.has_defaults = true;
        if (def["doorMode"]) {
            if (def["doorMode"]["doorLength"]) {
                def_dl = def["doorMode"]["doorLength"].as<int>();
            }
            if (def["doorMode"]["cornerDistance"]) {
                def_cd = def["doorMode"]["cornerDistance"].as<int>();
            }
        }
    }
    set.default_door_length = def_dl;
    set.default_corner_distance = def_cd;

    if (const auto& rds = root["roomDescriptions"]) {
        for (const auto& kv : rds) {
            set.rooms.push_back(parse_room_entry(kv.first.as<std::string>(), kv.second, def_dl, def_cd));
        }
    }

    return set;
}

PresetMap map_from_yaml_root(const YAML::Node& root, const std::string& filename, const std::string& display_name) {
    PresetMap map;
    map.filename = filename;
    map.display_name = display_name;

    if (root["roomsRange"]) {
        map.room_from = root["roomsRange"]["from"].as<int>();
        map.room_to = root["roomsRange"]["to"].as<int>();
    }

    if (root["passages"]) {
        for (const auto& p : root["passages"]) {
            map.passages.push_back({p[0].as<int>(), p[1].as<int>()});
        }
    }

    if (root["rooms"]) {
        for (const auto& kv : root["rooms"]) {
            PresetMap::RoomOverride ov;
            const std::string ids = room_override_key_to_ids_string(kv.first);
            std::stringstream ss(ids);
            std::string token;
            while (std::getline(ss, token, ',')) {
                int val = 0;
                bool neg = false;
                std::string trimmed;
                for (char c : token) {
                    if (c == '-') {
                        neg = true;
                    } else if (c == ' ' || c == '[' || c == ']') {
                    } else {
                        trimmed += c;
                    }
                }
                for (char c : trimmed) {
                    val = val * 10 + (c - '0');
                }
                if (neg) {
                    val = -val;
                }
                ov.room_ids.push_back(val);
            }
            const auto& shapes = kv.second["roomShapes"];
            if (shapes) {
                for (const auto& s : shapes) {
                    if (s.IsMap()) {
                        if (s["roomDescriptionName"]) {
                            ov.room_description_names.push_back(s["roomDescriptionName"].as<std::string>());
                        }
                    }
                }
            }
            map.room_overrides.push_back(std::move(ov));
        }
    }

    if (root["defaultRoomShapes"]) {
        for (const auto& s : root["defaultRoomShapes"]) {
            PresetMap::DefaultRoomShapes drs;
            if (s.IsMap()) {
                drs.set_name = s["setName"].as<std::string>();
                if (s["roomDescriptionName"]) {
                    drs.room_description_name = s["roomDescriptionName"].as<std::string>();
                }
                if (s["scale"]) {
                    for (const auto& v : s["scale"]) {
                        drs.scale.push_back(v.as<int>());
                    }
                }
            } else if (s.IsScalar()) {
                drs.set_name = s.as<std::string>();
            }
            map.default_room_shapes.push_back(std::move(drs));
        }
    }

    if (root["customRoomDescriptionsSet"]) {
        map.has_custom_descriptions = true;
        const auto& crd = root["customRoomDescriptionsSet"];
        int cdl = 1, ccd = 1;
        if (const auto& def = crd["default"]) {
            if (def["doorMode"]) {
                if (def["doorMode"]["doorLength"]) {
                    cdl = def["doorMode"]["doorLength"].as<int>();
                }
                if (def["doorMode"]["cornerDistance"]) {
                    ccd = def["doorMode"]["cornerDistance"].as<int>();
                }
            }
        }
        map.custom_descriptions.default_door_length = cdl;
        map.custom_descriptions.default_corner_distance = ccd;
        map.custom_descriptions.has_defaults = true;
        if (const auto& rds = crd["roomDescriptions"]) {
            for (const auto& kv : rds) {
                map.custom_descriptions.rooms.push_back(
                    parse_room_entry(kv.first.as<std::string>(), kv.second, cdl, ccd));
            }
        }
    }

    if (root["corridors"]) {
        const auto& corr = root["corridors"];
        map.corridors_enabled = corr["enable"] ? corr["enable"].as<bool>() : false;
        if (corr["offsets"]) {
            for (const auto& o : corr["offsets"]) {
                map.corridor_offsets.push_back(o.as<int>());
            }
        }
        if (corr["corridorShapes"]) {
            for (const auto& cs : corr["corridorShapes"]) {
                PresetMap::DefaultRoomShapes drs;
                if (cs.IsMap()) {
                    drs.set_name = cs["setName"].as<std::string>();
                } else if (cs.IsScalar()) {
                    drs.set_name = cs.as<std::string>();
                }
                map.corridor_shapes.push_back(std::move(drs));
            }
        }
    }

    return map;
}

std::filesystem::path resolve_resource_base_for_map(const std::filesystem::path& map_file, std::string& err) {
    namespace fs = std::filesystem;
    const fs::path parent = map_file.parent_path();
    if (parent.filename() == "Maps") {
        return parent.parent_path();
    }
    const fs::path rooms_sibling = parent / "Rooms";
    if (fs::exists(rooms_sibling) && fs::is_directory(rooms_sibling)) {
        return parent;
    }
    const fs::path rooms_up = parent.parent_path() / "Rooms";
    if (fs::exists(rooms_up) && fs::is_directory(rooms_up)) {
        return parent.parent_path();
    }
    err = "Cannot find resource root: put the map under .../Maps/name.yml or next to a Rooms/ folder.";
    return {};
}

} // namespace

PresetCatalogLoadResult load_preset_catalog_with_status(const std::string& base_path) {
    PresetCatalogLoadResult out;
    namespace fs = std::filesystem;
    try {
        const fs::path base(base_path);
        if (!fs::exists(base)) {
            out.error = "Resource path does not exist: " + base_path;
            return out;
        }
        if (!fs::is_directory(base)) {
            out.error = "Resource path is not a directory: " + base_path;
            return out;
        }

        out.catalog.base_path = base_path;
        const fs::path rooms_dir = base / "Rooms";
        if (fs::exists(rooms_dir) && fs::is_directory(rooms_dir)) {
            for (const auto& entry : fs::directory_iterator(rooms_dir)) {
                if (entry.path().extension() == ".yml") {
                    try {
                        auto rs = load_room_set(entry.path().string());
                        if (!rs.rooms.empty()) {
                            out.catalog.room_sets.push_back(std::move(rs));
                        }
                    } catch (const std::exception&) {
                        continue;
                    }
                }
            }
        }

        const fs::path maps_dir = base / "Maps";
        if (!fs::exists(maps_dir) || !fs::is_directory(maps_dir)) {
            out.error = "No Maps/ directory under: " + base_path;
            return out;
        }

        // Only YAML files directly in Maps/ (not in subfolders like Maps/Thesis/).
        for (const auto& entry : fs::directory_iterator(maps_dir)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const auto& path = entry.path();
            const auto ext = path.extension();
            if (ext != ".yml" && ext != ".yaml") {
                continue;
            }
            std::ifstream f(path.string());
            if (!f.is_open()) {
                continue;
            }
            YAML::Node root;
            try {
                root = YAML::Load(f);
            } catch (const std::exception&) {
                // Skip maps with invalid YAML (e.g. legacy C#-style keys); keep loading others.
                continue;
            }
            try {
                const fs::path rel = fs::relative(path, maps_dir);
                const std::string file_id = rel.generic_string();
                fs::path rel_display = rel;
                rel_display.replace_extension("");
                const std::string display_name = rel_display.generic_string();
                PresetMap map =
                    map_from_yaml_root(root, file_id, display_name.empty() ? rel.stem().string() : display_name);
                out.catalog.maps.push_back(std::move(map));
            } catch (const std::exception&) {
                continue;
            }
        }

        std::sort(out.catalog.maps.begin(), out.catalog.maps.end(),
                  [](const PresetMap& a, const PresetMap& b) { return a.display_name < b.display_name; });
        std::sort(out.catalog.room_sets.begin(), out.catalog.room_sets.end(),
                  [](const PresetRoomSet& a, const PresetRoomSet& b) { return a.name < b.name; });
    } catch (const std::exception& e) {
        out.error = std::string("Failed to load catalog: ") + e.what();
    }
    return out;
}

PresetCatalogLoadResult load_preset_catalog_from_map_file(const std::string& map_yml_path) {
    PresetCatalogLoadResult out;
    namespace fs = std::filesystem;
    try {
        const fs::path map_file = fs::absolute(map_yml_path);
        if (!fs::exists(map_file)) {
            out.error = "Map file does not exist: " + map_yml_path;
            return out;
        }
        std::string resolve_err;
        fs::path base = resolve_resource_base_for_map(map_file, resolve_err);
        if (base.empty()) {
            out.error = resolve_err;
            return out;
        }

        PresetCatalogLoadResult full = load_preset_catalog_with_status(base.string());
        if (!full.error.empty()) {
            return full;
        }

        const fs::path maps_dir = base / "Maps";
        std::error_code rec;
        const fs::path rel_to_maps = fs::relative(map_file, maps_dir, rec);
        const std::string target_key =
            (!rec && !rel_to_maps.empty()) ? rel_to_maps.generic_string() : map_file.filename().string();

        std::vector<PresetMap> filtered;
        for (auto& m : full.catalog.maps) {
            if (m.filename == target_key || m.filename == map_file.filename().string()) {
                filtered.push_back(std::move(m));
            }
        }

        if (!filtered.empty()) {
            full.catalog.maps = std::move(filtered);
            return full;
        }

        // Parse standalone file (e.g. filename mismatch or not under scanned Maps/)
        std::ifstream f(map_file.string());
        if (!f.is_open()) {
            out.error = "Cannot open: " + map_file.string();
            return out;
        }
        YAML::Node root = YAML::Load(f);
        const std::string file_id =
            (!rec && !rel_to_maps.empty()) ? rel_to_maps.generic_string() : map_file.filename().string();
        std::string display_name;
        if (!rec && !rel_to_maps.empty()) {
            fs::path d = rel_to_maps;
            d.replace_extension("");
            display_name = d.generic_string();
        } else {
            display_name = map_file.stem().string();
        }
        PresetMap map = map_from_yaml_root(root, file_id, display_name);
        full.catalog.maps = {std::move(map)};
        full.error.clear();
        return full;
    } catch (const std::exception& e) {
        out.error = std::string("load_preset_catalog_from_map_file: ") + e.what();
        return out;
    }
}

PresetCatalog load_preset_catalog(const std::string& base_path) {
    auto r = load_preset_catalog_with_status(base_path);
    if (!r.error.empty()) {
        return PresetCatalog{};
    }
    return std::move(r.catalog);
}

static const PresetRoomSet::RoomEntry* find_room_entry(
    const std::vector<PresetRoomSet>& sets, const std::string& set_name, const std::string& entry_name) {
    for (const auto& rs : sets) {
        if (rs.name == set_name) {
            for (const auto& r : rs.rooms) {
                if (r.name == entry_name) {
                    return &r;
                }
            }
        }
    }
    return nullptr;
}

static RoomTemplateGrid2D build_room_template(const PresetRoomSet::RoomEntry& entry) {
    std::vector<edgar::geometry::Vector2Int> pts = entry.shape;
    edgar::geometry::PolygonGrid2D poly(pts);

    std::shared_ptr<IDoorModeGrid2D> door_mode;
    if (entry.door_mode == "SpecificPositionsMode" && !entry.specific_doors.empty()) {
        std::vector<DoorGrid2D> doors;
        doors.reserve(entry.specific_doors.size());
        for (const auto& [from, to] : entry.specific_doors) {
            doors.push_back(DoorGrid2D{.from = from, .to = to});
        }
        door_mode = std::make_shared<ManualDoorModeGrid2D>(std::move(doors));
    } else {
        door_mode = std::make_shared<SimpleDoorModeGrid2D>(entry.door_length, entry.corner_distance);
    }
    return RoomTemplateGrid2D(std::move(poly), std::move(door_mode), entry.name);
}

LevelDescriptionGrid2D<int> build_level_from_preset(const PresetMap& map, const std::vector<PresetRoomSet>& room_sets) {

    LevelDescriptionGrid2D<int> level;

    std::unordered_map<int, std::vector<RoomTemplateGrid2D>> room_templates;

    for (const auto& ov : map.room_overrides) {
        std::vector<RoomTemplateGrid2D> templates;
        for (const auto& rn : ov.room_description_names) {
            const PresetRoomSet::RoomEntry* found = nullptr;
            if (map.has_custom_descriptions) {
                for (const auto& r : map.custom_descriptions.rooms) {
                    if (r.name == rn) {
                        found = &r;
                        break;
                    }
                }
            }
            if (!found) {
                for (const auto& drs : map.default_room_shapes) {
                    found = find_room_entry(room_sets, drs.set_name, rn);
                    if (found) {
                        break;
                    }
                }
            }
            if (found) {
                templates.push_back(build_room_template(*found));
            }
        }
        for (int id : ov.room_ids) {
            room_templates[id] = templates;
        }
    }

    for (const auto& drs : map.default_room_shapes) {
        if (!drs.room_description_name.empty()) {
            continue;
        }
        const PresetRoomSet* matching_set = nullptr;
        for (const auto& rs : room_sets) {
            if (rs.name == drs.set_name) {
                matching_set = &rs;
                break;
            }
        }
        if (!matching_set) {
            continue;
        }
        for (int id = map.room_from; id <= map.room_to; ++id) {
            if (room_templates.count(id)) {
                continue;
            }
            std::vector<RoomTemplateGrid2D> templates;
            for (const auto& r : matching_set->rooms) {
                templates.push_back(build_room_template(r));
            }
            room_templates[id] = std::move(templates);
        }
    }

    for (int id = map.room_from; id <= map.room_to; ++id) {
        auto it = room_templates.find(id);
        if (it != room_templates.end() && !it->second.empty()) {
            RoomDescriptionGrid2D desc(false, it->second);
            level.add_room(id, desc);
        } else {
            auto square = RoomTemplateGrid2D(edgar::geometry::PolygonGrid2D::get_square(8),
                                             std::make_shared<SimpleDoorModeGrid2D>(1, 1));
            RoomDescriptionGrid2D desc(false, {square});
            level.add_room(id, desc);
        }
    }

    for (const auto& p : map.passages) {
        level.add_connection(p.a, p.b);
    }

    return level;
}

LevelDescriptionGrid2D<int> build_level_from_preset(const PresetMap& map, const PresetCatalog& catalog) {
    return build_level_from_preset(map, catalog.room_sets);
}

} // namespace edgar::generator::grid2d
