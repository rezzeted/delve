#include "assets.h"

#include <filesystem>

#if defined(__APPLE__)
    #include <mach-o/dyld.h>
#endif

namespace delve {

namespace {

namespace fs = std::filesystem;

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

std::string find_delve_assets(const std::string& argv0, const std::string& projectPath) {
    std::error_code ec;
    if (isAssetsDir(fs::current_path(ec) / "assets"))
        return (fs::current_path(ec) / "assets").string();
    if (std::string found = findAssetsUp(exePath(argv0).parent_path()); !found.empty()) return found;
    if (!projectPath.empty())
        if (std::string found = findAssetsUp(fs::path(projectPath).parent_path()); !found.empty())
            return found;
    return {};
}

}  // namespace delve
