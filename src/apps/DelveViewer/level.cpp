#include "pch.h"

#include "level.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#if defined(__APPLE__)
    #include <mach-o/dyld.h>
#endif

#include "catalog.h"
#include "generate.h"

namespace {

namespace fs = std::filesystem;

constexpr int kLayoutAttempts = 4;

double nowMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool readTextFile(const std::string& path, std::string& text) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    text = ss.str();
    return true;
}

// Best-effort path of the running executable (for the assets walk-up).
fs::path exePath(const std::string& argv0) {
#if defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) return fs::path(buf);
    return fs::path(argv0);
#elif defined(_WIN32)
    // No GetModuleFileName here (keeps <windows.h> out); argv[0] plus the cwd
    // and project-dir fallbacks cover the dev flows.
    return fs::path(argv0);
#else
    std::error_code ec;
    const fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path(argv0) : p;
#endif
}

// The delve assets library root carries the shared code tables.
bool isAssetsDir(const fs::path& dir) {
    std::error_code ec;
    return fs::is_regular_file(dir / "codes.pgg", ec);
}

// dir, then up to 12 parents: <dir>/assets with codes.pgg wins; a dir that is
// itself the assets root is accepted too.
std::string findAssetsUp(fs::path dir) {
    std::error_code ec;
    dir = fs::weakly_canonical(dir, ec);
    if (ec) return {};
    for (int i = 0; i < 12; ++i) {
        if (isAssetsDir(dir / "assets")) return (dir / "assets").string();
        if (isAssetsDir(dir)) return dir.string();
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    return {};
}

}  // namespace

std::string resolve_delve_assets(const std::string& argv0, const std::string& projectPath) {
    std::error_code ec;
    if (isAssetsDir(fs::current_path(ec) / "assets"))
        return (fs::current_path(ec) / "assets").string();
    if (std::string found = findAssetsUp(exePath(argv0).parent_path()); !found.empty()) return found;
    if (!projectPath.empty())
        if (std::string found = findAssetsUp(fs::path(projectPath).parent_path()); !found.empty())
            return found;
    return {};
}

bool Level::readFrozenIr(const std::string& path, std::string& err) {
    std::string text;
    if (!readTextFile(path, text)) {
        err = "cannot open " + path;
        return false;
    }
    // delve-ir/0 (D0 frozen) builds through the project; delve-ir/2|3 (F5
    // artifact) reads directly. The format key decides (N7: reject the rest).
    const nlohmann::json doc = nlohmann::json::parse(text, nullptr, false);
    const std::string format = doc.is_object() ? doc.value("format", std::string{}) : std::string{};
    if (format == "delve-ir/0")
        return delve::build_ir_v2(text, path, project, projectPath, ir, err);
    if (format == delve::kIrFormat || format == delve::kIrFormatV2)
        return delve::read_ir_v2_json(text, ir, err);
    err = path + ": unsupported IR format \"" + format + "\" (expected delve-ir/0, " +
          delve::kIrFormatV2 + " or " + delve::kIrFormat + ")";
    return false;
}

bool Level::buildIrFromGenerate(std::string& err) {
    if (!project.layout) {
        err = projectPath + ": delve-project/0 has no layout tier; pass --ir <frozen.json>";
        return false;
    }
    delve::layout::Catalog catalog;
    if (!delve::layout::build_catalog(project, catalog, err)) return false;
    delve::layout::LayoutGenerator gen;
    delve::layout::GenerateOptions opts;
    opts.attempts = kLayoutAttempts;
    delve::layout::LayoutResult result;
    const double t0 = nowMs();
    if (!gen.generate(project, catalog, opts, result, err)) return false;
    layoutMs = nowMs() - t0;
    // The generator speaks edgar types; delve-layout/0 is the canonical
    // handoff into the IR builder (serialize, then parse back).
    std::string text;
    if (!delve::layout::write_layout_json(result, project, projectPath, text, err)) return false;
    if (!delve::read_layout_json(text, layoutData, err)) return false;
    return delve::build_ir_from_layout(layoutData, project, projectPath, ir, err);
}

bool Level::runFill(std::string& err) {
    delve::FillOpts opts;
    opts.delve_assets = delveAssets;
    opts.cache = &unitCache;
    const double t0 = nowMs();
    if (!delve::fill_level(ir, project, opts, fill, err)) return false;
    fillMs = nowMs() - t0;
    return true;
}

bool Level::load(const std::string& proj, const std::string& irFile, const std::string& assets,
                 std::string& err) {
    if (assets.empty()) {
        err = "cannot locate the delve assets dir (no assets/codes.pgg from the cwd, the "
              "executable or the project dir upwards)";
        return false;
    }
    Level next;
    next.unitCache = std::move(unitCache);  // F8: the cache outlives reloads
    next.projectPath = proj;
    next.irPath = irFile;
    next.delveAssets = assets;
    const bool ok = delve::load_project(proj, next.project, err) &&
                    (irFile.empty() ? next.buildIrFromGenerate(err) : next.readFrozenIr(irFile, err)) &&
                    next.runFill(err);
    if (!ok) {
        unitCache = std::move(next.unitCache);  // keep the cache on failure too
        return false;
    }
    next.loaded = true;
    *this = std::move(next);
    return true;
}

bool Level::refill(std::string& err) {
    if (!loaded) {
        err = "no level loaded";
        return false;
    }
    if (!delve::load_project(projectPath, project, err)) return false;
    // Same layout, fresh resolution: rebuild the IR from the stored layout
    // (or re-read the frozen file), then refill through the shared cache.
    const bool irOk = irPath.empty()
                          ? delve::build_ir_from_layout(layoutData, project, projectPath, ir, err)
                          : readFrozenIr(irPath, err);
    return irOk && runFill(err);
}

bool Level::relayout(std::string& err) {
    if (!loaded) {
        err = "no level loaded";
        return false;
    }
    if (!irPath.empty()) {
        err = "the level comes from --ir; re-layout needs a layout-tier project";
        return false;
    }
    if (!delve::load_project(projectPath, project, err)) return false;
    return buildIrFromGenerate(err) && runFill(err);
}
