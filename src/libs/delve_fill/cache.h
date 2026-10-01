#pragma once

// Delve F8: unit-output cache. The key mixes the slot kind, the unit's
// bindings (pgg::fingerprintValue per value; rng_seed is an ordinary binding)
// and the asset's content together with its import closure (R-A7). Outputs
// are stored in the unit's local frame; world placement is applied at
// assembly (fill.cpp), so a hit is position-independent by construction.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pgg/eval.h"

namespace delve {

// Key-composition schema; bump when the key contents change.
inline constexpr uint64_t kUnitKeyVersion = 1;

struct UnitKey {
    uint64_t h = 0;
    bool operator==(const UnitKey&) const = default;
};

// Content key of one slot's asset (R-A7): bytes of the asset file plus bytes
// of every file of its transitive import closure (canonical paths sorted and
// fed into the hash too). Import roots mirror check_asset: the asset's own
// directory, then import_roots, then the product lib root. Computed once per
// fill_level call per slot.
bool asset_content_key(const std::string& asset_path,
                       const std::vector<std::string>& import_roots, uint64_t& out,
                       std::string& err);

// Key of one unit: mix(kUnitKeyVersion, slot, asset_key, per-binding name +
// pgg::fingerprintValue(value)). False + err when a binding value has no
// structural fingerprint (uncacheable payload).
bool unit_key(const std::string& slot, uint64_t asset_key,
              const std::vector<std::pair<std::string, pgg::Value>>& bindings, UnitKey& out,
              std::string& err);

// In-memory unit-output cache owned by the caller (like RunParams::cache in
// PGG). Stored geos are in the unit's local frame and immutable after store
// (assembly copies before transforming), so lookup shares them. Unbounded:
// levels are a few hundred units; a disk layer with GC is a D4 concern.
class UnitCache {
  public:
    struct Entry {
        pgg::GeoPtr mesh;
        pgg::GeoPtr anchors;
    };
    bool lookup(const UnitKey& k, Entry& out) const {
        const auto it = map_.find(k.h);
        if (it == map_.end()) return false;
        out = it->second;
        return true;
    }
    void store(const UnitKey& k, pgg::GeoPtr mesh, pgg::GeoPtr anchors) {
        map_[k.h] = Entry{std::move(mesh), std::move(anchors)};
    }
    size_t size() const { return map_.size(); }
    void clear() { map_.clear(); }

  private:
    std::unordered_map<uint64_t, Entry> map_;
};

}  // namespace delve
