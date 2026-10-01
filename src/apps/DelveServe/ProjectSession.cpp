#include "ProjectSession.h"

#include <chrono>
#include <filesystem>

std::string canonicalServePath(const std::string& path) {
    std::error_code ec;
    const std::filesystem::path canon =
        std::filesystem::weakly_canonical(std::filesystem::path(path), ec);
    return ec ? path : canon.string();
}

std::int64_t fileMtimeNs(const std::string& path) {
    std::error_code ec;
    const std::filesystem::file_time_type t = std::filesystem::last_write_time(path, ec);
    if (ec) return 0;
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

double wallNowSec() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
