#pragma once

// Locating the delve asset library (the dir carrying codes.pgg) for hosts that
// run the pipeline (DelveCli, DelveServe). Search order, same as DelveViewer:
// ./assets from the cwd, then up from the executable, then up from the
// project dir. Empty string when nothing matched — the caller decides the
// error (DelveCli reports D100 with a --assets hint).

#include <string>

namespace delve {

std::string find_delve_assets(const std::string& argv0, const std::string& projectPath);

}  // namespace delve
