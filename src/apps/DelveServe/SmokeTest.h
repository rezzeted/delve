#pragma once

// DelveServe --smoke (docs/mcp_v1.md): a real server on 127.0.0.1:0 pumped by
// a poll thread, real TCP clients from the same process. Covers the RPC
// contract, the warm F8 loop, mtime reload with partial cache invalidation
// and two concurrent clients. Relative fixture paths want the repo root as
// the working directory (ctest sets it). True = all checks passed.
#include <string>

bool runDelveServeSmokeTest(const std::string& argv0, const std::string& assetsDir);
