#include "pch.h"

#include "level.h"

#include <chrono>
#include <filesystem>

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

std::string resolve_delve_projects(const std::string& argv0) {
    const std::string assets = resolve_delve_assets(argv0, "");
    if (assets.empty()) return {};
    std::error_code ec;
    const fs::path projects = fs::path(assets).parent_path() / "projects";
    return fs::is_directory(projects, ec) ? projects.string() : std::string{};
}

bool Level::buildIrFromGenerate(std::string& err) {
    if (!project.layout) {
        err = projectPath + ": delve-project/0 has no layout tier to generate from";
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

bool Level::load(const std::string& proj, const std::string& assets, std::string& err) {
    if (assets.empty()) {
        err = "cannot locate the delve assets dir (no assets/codes.pgg from the cwd, the "
              "executable or the project dir upwards)";
        return false;
    }
    Level next;
    next.unitCache = std::move(unitCache);  // F8: the cache outlives reloads
    next.projectPath = proj;
    next.delveAssets = assets;
    const bool ok =
        delve::load_project(proj, next.project, err) && next.buildIrFromGenerate(err) &&
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
    // Same layout, fresh resolution: rebuild the IR from the stored layout,
    // then refill through the shared cache.
    return delve::build_ir_from_layout(layoutData, project, projectPath, ir, err) && runFill(err);
}

bool Level::relayout(std::string& err) {
    if (!loaded) {
        err = "no level loaded";
        return false;
    }
    if (!delve::load_project(projectPath, project, err)) return false;
    return buildIrFromGenerate(err) && runFill(err);
}
