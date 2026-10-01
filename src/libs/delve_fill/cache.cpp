// Delve F8: unit cache keys (see cache.h).

#include "cache.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "pgg/src/eval/fingerprint.h"
#include "pgg/src/eval/modules.h"

namespace delve {
namespace {

constexpr uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

uint64_t fnv1a(const void* data, size_t n, uint64_t h) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= kFnvPrime;
    }
    return h;
}

uint64_t fnv1a(const std::string& s, uint64_t h) { return fnv1a(s.data(), s.size(), h); }

uint64_t mixU64(uint64_t v, uint64_t h) { return fnv1a(&v, sizeof(v), h); }

bool hashFileBytes(const std::string& path, uint64_t& h, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "delve/run: cannot read asset file '" + path + "'";
        return false;
    }
    std::ostringstream s;
    s << in.rdbuf();
    const std::string bytes = s.str();
    h = fnv1a(bytes, h);
    return true;
}

}  // namespace

bool asset_content_key(const std::string& asset_path,
                       const std::vector<std::string>& import_roots, uint64_t& out,
                       std::string& err) {
    uint64_t h = kFnvOffset;
    if (!hashFileBytes(asset_path, h, err)) return false;

    const pgg::Document doc = pgg::parseFile(asset_path);
    if (!doc.file || doc.hasErrors()) {
        err = "delve/run: asset '" + asset_path + "' does not parse";
        return false;
    }
    if (!pgg::hasImports(*doc.file)) {
        out = h;
        return true;
    }
    std::vector<std::string> roots;
    const std::string dir = std::filesystem::path(asset_path).parent_path().string();
    if (!dir.empty()) roots.push_back(dir);
    for (const auto& r : import_roots) roots.push_back(r);
    pgg::appendImportRoot(roots, pgg::findProductLibRoot(asset_path));
    std::vector<pgg::Diagnostic> diags;
    const pgg::ModuleClosure closure = pgg::loadModuleClosure(*doc.file, roots, diags);
    bool errors = false;
    for (const auto& d : diags) errors = errors || !d.isWarning;
    if (errors) {
        err = "delve/run: imports of '" + asset_path + "' fail:";
        for (const auto& d : diags)
            if (!d.isWarning) err += "\n  [" + d.code + "] " + d.message;
        return false;
    }
    std::vector<std::string> paths;
    for (const pgg::ModuleInfo* m : closure.modules) paths.push_back(m->canonicalPath);
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    for (const std::string& p : paths) {
        h = fnv1a(p, h);
        if (!hashFileBytes(p, h, err)) return false;
    }
    out = h;
    return true;
}

bool unit_key(const std::string& slot, uint64_t asset_key,
              const std::vector<std::pair<std::string, pgg::Value>>& bindings, UnitKey& out,
              std::string& err) {
    uint64_t h = kFnvOffset;
    h = mixU64(kUnitKeyVersion, h);
    h = fnv1a(slot, h);
    h = mixU64(asset_key, h);
    for (const auto& [name, value] : bindings) {
        uint64_t vh = 0;
        if (!pgg::fingerprintValue(value, vh)) {
            err = "delve/run: binding '" + name + "' has no structural fingerprint";
            return false;
        }
        h = fnv1a(name, h);
        h = mixU64(vh, h);
    }
    out = UnitKey{h};
    return true;
}

}  // namespace delve
