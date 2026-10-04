#include "SmokeTest.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "ProjectSession.h"
#include "ServeRpcServer.h"
#include "ServeRuntime.h"

#if defined(_WIN32)
    #include <process.h>
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <sys/time.h>
    #include <unistd.h>
#endif

namespace {

int g_failures = 0;

void check(bool ok, const char* name) {
    if (ok) {
        spdlog::info("TEST PASS: {}", name);
    } else {
        spdlog::error("TEST FAIL: {}", name);
        ++g_failures;
    }
}

#if defined(_WIN32)
using SocketFd = SOCKET;
static constexpr SocketFd kBadSock = INVALID_SOCKET;
static void closeFd(SocketFd fd) { closesocket(fd); }
static int smokePid() { return _getpid(); }
#else
using SocketFd = int;
static constexpr SocketFd kBadSock = -1;
static void closeFd(SocketFd fd) { close(fd); }
static int smokePid() { return getpid(); }
#endif

SocketFd connectClient(const std::string& host, uint16_t port) {
#if defined(_WIN32)
    SOCKET cfd = socket(AF_INET, SOCK_STREAM, 0);
    if (cfd == INVALID_SOCKET) return kBadSock;
    const DWORD recvTimeoutMs = 120000;
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&recvTimeoutMs),
               sizeof(recvTimeoutMs));
#else
    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    if (cfd < 0) return kBadSock;
    const timeval recvTimeout{120, 0};
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &recvTimeout, sizeof(recvTimeout));
#if defined(SO_NOSIGPIPE)
    const int one = 1;
    setsockopt(cfd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
#endif
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    if (connect(cfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closeFd(cfd);
        return kBadSock;
    }
    return cfd;
}

nlohmann::json readReply(SocketFd fd, std::string& inbuf) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (std::chrono::steady_clock::now() < deadline) {
        const size_t nl = inbuf.find('\n');
        if (nl != std::string::npos) {
            const std::string resp = inbuf.substr(0, nl);
            inbuf.erase(0, nl + 1);
            return nlohmann::json::parse(resp, nullptr, false);
        }
        char buf[65536];
#if defined(_WIN32)
        const int n = recv(fd, buf, sizeof(buf), 0);
#else
        const ssize_t n = recv(fd, buf, sizeof(buf), 0);
#endif
        if (n > 0) {
            inbuf.append(buf, static_cast<size_t>(n));
        } else if (n == 0) {
            break;
        }
    }
    return nlohmann::json{};
}

nlohmann::json rpcCall(SocketFd fd, const nlohmann::json& req, std::string& inbuf) {
    const std::string line = req.dump() + "\n";
    if (send(fd, line.data(), static_cast<int>(line.size()), 0) < 0) return nlohmann::json{};
    return readReply(fd, inbuf);
}

nlohmann::json rawCall(SocketFd fd, const std::string& text, std::string& inbuf) {
    const std::string line = text + "\n";
    if (send(fd, line.data(), static_cast<int>(line.size()), 0) < 0) return nlohmann::json{};
    return readReply(fd, inbuf);
}

std::string readFileText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void writeFileText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

}  // namespace

bool runDelveServeSmokeTest(const std::string& argv0, const std::string& assetsDir) {
    (void)argv0;
    g_failures = 0;
    namespace fs = std::filesystem;
    using nlohmann::json;

    ServeRpcServer server;
    ServeRuntime runtime(server);
    runtime.setAssetsDir(assetsDir);
    runtime.startTimeSec = wallNowSec();
    runtime.startWorkers();
    runtime.registerHandlers();
    check(server.start("127.0.0.1", 0), "rpc server starts on an ephemeral port");
    const uint16_t port = server.listenPort();
    check(port != 0, "ephemeral listen port is non-zero");

    std::atomic<bool> polling{true};
    std::thread poller([&] {
        while (polling.load()) {
            server.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    const fs::path tmpDir =
        fs::temp_directory_path() / ("delve_serve_smoke_" + std::to_string(smokePid()));
    std::error_code ec;
    fs::create_directories(tmpDir, ec);

    std::string inbuf;
    SocketFd cfd = connectClient("127.0.0.1", port);
    check(cfd != kBadSock, "rpc client connects");
    auto call = [&](const json& req) { return rpcCall(cfd, req, inbuf); };

    fs::path copyPath;        // step 8: editable copy of the smoke project
    std::string copyCanonical;

    if (cfd != kBadSock) {
        // 1. ping / status
        const json pong = call({{"op", "ping"}, {"args", json::object()}});
        check(pong.value("ok", false) && pong["data"].value("pong", false) &&
                  pong["data"].value("app", std::string{}) == "DelveServe" &&
                  pong["data"].value("protocol", 0) == 1,
              "rpc ping -> pong app=DelveServe protocol=1");
        const json status0 = call({{"op", "status"}, {"args", json::object()}});
        check(status0.value("ok", false) && status0["data"]["slots"].is_array() &&
                  status0["data"]["slots"].empty() &&
                  status0["data"].value("port", 0) > 0 &&
                  !status0["data"].value("assets_dir", std::string{}).empty(),
              "rpc status: 0 slots, port>0, assets_dir set");

        // 2. protocol errors
        const json badJson = rawCall(cfd, "{ not json", inbuf);
        check(!badJson.value("ok", true) &&
                  badJson["error"].value("kind", std::string{}) == "bad_json",
              "rpc invalid JSON -> bad_json");
        const json unknown = call({{"op", "no_such_op"}, {"args", json::object()}});
        check(!unknown.value("ok", true) &&
                  unknown["error"].value("kind", std::string{}) == "unknown_op",
              "rpc unknown op -> unknown_op");

        // 3./4. load failures are ok:true with classified diagnostics, no slot
        const json noSuch = call({{"op", "load"}, {"args", {{"path", "no/such/project.json"}}}});
        check(noSuch.value("ok", false) && noSuch["data"].value("has_errors", false) &&
                  !noSuch["data"]["diagnostics"].empty() &&
                  noSuch["data"]["diagnostics"][0].value("code", std::string{}) == "D100",
              "rpc load of a missing project -> ok:true D100");
        const json bad = call(
            {{"op", "load"}, {"args", {{"path", "src/tests/data/d4_bad_project.json"}}}});
        check(bad.value("ok", false) && bad["data"].value("has_errors", false) &&
                  !bad["data"]["diagnostics"].empty() &&
                  bad["data"]["diagnostics"][0].value("code", std::string{}) == "D101",
              "rpc load of d4_bad_project.json -> ok:true D101");

        // 5. load the smoke project (3 rooms: entry/hall/c1)
        const json good =
            call({{"op", "load"}, {"args", {{"path", "src/apps/DelveViewer/smoke_project.json"}}}});
        check(good.value("ok", false) && !good["data"].value("has_errors", true) &&
                  good["data"]["session"].contains("file"),
              "rpc load of smoke_project.json");
        const std::string smokeFile = good["data"]["session"].value("file", std::string{});

        // 6. first fill (auto-layout included) through the clientFile fallback
        const json fill1 = call({{"op", "fill"}, {"args", json::object()}});
        bool fill1Ok = fill1.value("ok", false);
        size_t totalUnits = 0;
        if (fill1Ok) {
            const json& d = fill1["data"];
            totalUnits = d.value("rooms", 0u) + d.value("bodies", 0u) + d.value("facings", 0u) +
                         d.value("nodes", 0u) + d.value("doors", 0u) + d.value("lamps", 0u);
            fill1Ok = d.value("rooms", 0u) == 3u && d.value("reused", 1u) == 0u &&
                      d.value("reran", 0u) == totalUnits && totalUnits > 0 &&
                      d.value("note", std::string{}) == "used the client current file" &&
                      d["session"].value("file", std::string{}) == smokeFile;
        }
        check(fill1Ok, "rpc fill #1 (clientFile fallback): rooms=3, all units reran, note echoed");

        // 7. the warm loop: everything comes from the F8 cache
        const json fill2 = call({{"op", "fill"}, {"args", json::object()}});
        check(fill2.value("ok", false) && fill2["data"].value("reran", 1u) == 0u &&
                  fill2["data"].value("reused", 0u) > 0u,
              "rpc fill #2 is fully warm (reran=0, reused>0)");

        // 8. mtime reload: edit a fill-tier param of a copy, partial invalidation
        copyPath = tmpDir / "smoke_copy.json";
        fs::copy_file("src/apps/DelveViewer/smoke_project.json", copyPath,
                      fs::copy_options::overwrite_existing, ec);
        const json cload = call({{"op", "load"}, {"args", {{"path", copyPath.string()}}}});
        check(cload.value("ok", false) && !cload["data"].value("has_errors", true),
              "reload: load of the tmp copy");
        copyCanonical = cload["data"]["session"].value("file", std::string{});
        const json cfill1 = call({{"op", "fill"}, {"args", json::object()}});
        check(cfill1.value("ok", false) && cfill1["data"].value("reran", 0u) > 0u,
              "reload: cold fill of the copy reran units");
        {
            json doc = json::parse(readFileText(copyPath), nullptr, false);
            doc["fill"]["door_h"] = 2.4;  // door units + the walls/facings they cut
            writeFileText(copyPath, doc.dump(2));
            fs::last_write_time(copyPath, fs::file_time_type::clock::now() + std::chrono::seconds(2),
                                ec);
        }
        const json cfill2 = call({{"op", "fill"}, {"args", json::object()}});
        check(cfill2.value("ok", false) && cfill2["data"].value("reused", 0u) > 0u &&
                  cfill2["data"].value("reran", 0u) > 0u,
              "reload: edited door_h -> partial invalidation (reused>0, reran>0)");
        const json st8 = call({{"op", "status"}, {"args", json::object()}});
        bool copyRowOk = false;
        if (st8.value("ok", false))
            for (const json& s : st8["data"]["slots"])
                if (s.value("file", std::string{}) == copyCanonical)
                    copyRowOk = s.value("has_layout", false);
        check(copyRowOk, "reload: the layout was kept, not regenerated (status has_layout)");

        // 9. F11 checks on the smoke slot
        const json chk = call({{"op", "check"}, {"args", {{"file", smokeFile}}}});
        check(chk.value("ok", false) && chk["data"].value("errors", 1u) == 0u &&
                  chk["data"].value("has_errors", true) == false,
              "rpc check on smoke_project -> errors=0");

        // 9b. B3: a second identical check replays cached elements verdicts —
        // same answer, and now from the verdict cache.
        const json chk2 = call({{"op", "check"}, {"args", {{"file", smokeFile}}}});
        check(chk2.value("ok", false) && chk2["data"].value("errors", 1u) == 0u &&
                  chk2["data"].value("has_errors", true) == false,
              "rpc check again (cached elements verdicts) -> errors=0");

        // 10. export artifacts
        const json exp = call(
            {{"op", "export"}, {"args", {{"file", smokeFile}, {"out", (tmpDir / "export").string()}}}});
        bool expOk = exp.value("ok", false) && exp["data"]["written"].size() == 4;
        if (expOk)
            for (const json& w : exp["data"]["written"])
                expOk = expOk && fs::is_regular_file(w.get<std::string>(), ec);
        check(expOk, "rpc export -> 4 artifacts on disk");

        // 11. unit spans of the last fill
        const json units = call({{"op", "units"}, {"args", {{"file", smokeFile}}}});
        bool unitsOk = units.value("ok", false) && !units["data"]["units"].empty();
        if (unitsOk)
            for (const json& u : units["data"]["units"])
                unitsOk = unitsOk && !u.value("id", std::string{}).empty() &&
                          !u.value("slot", std::string{}).empty();
        check(unitsOk, "rpc units -> non-empty spans with id+slot");

        // 12. F12 provenance chains of the hall
        const json prov =
            call({{"op", "provenance"}, {"args", {{"file", smokeFile}, {"room", "hall"}}}});
        check(prov.value("ok", false) && !prov["data"]["entries"].empty(),
              "rpc provenance room=hall -> non-empty entries");

        // 13. R-A3 static slot checks
        const json acGood = call(
            {{"op", "asset_check"}, {"args", {{"slot", "facing"}, {"asset", "walls/facing_v1.pgg"}}}});
        check(acGood.value("ok", false) && !acGood["data"].value("has_errors", true),
              "rpc asset_check facing/walls/facing_v1.pgg -> clean");
        const json acBad = call(
            {{"op", "asset_check"}, {"args", {{"slot", "facing"}, {"asset", "doors/opening_v1.pgg"}}}});
        bool slotCodeSeen = false;
        if (acBad.value("ok", false))
            for (const json& d : acBad["data"]["diagnostics"])
                slotCodeSeen = slotCodeSeen ||
                               d.value("code", std::string{}).rfind("delve/slot", 0) == 0;
        check(acBad.value("ok", false) && acBad["data"].value("has_errors", false) && slotCodeSeen,
              "rpc asset_check facing/doors/opening_v1.pgg -> delve/slot diagnostics");
    }

    // 14. two TCP clients, two slots, concurrent fills through the worker pool
    {
        const fs::path copy2 = tmpDir / "smoke_copy2.json";
        fs::copy_file(copyPath, copy2, fs::copy_options::overwrite_existing, ec);
        std::atomic<bool> aOk{false}, bOk{false};
        std::thread ta([&] {
            std::string buf;
            SocketFd a = connectClient("127.0.0.1", port);
            if (a == kBadSock) return;
            const json l = rpcCall(
                a, {{"op", "load"}, {"args", {{"path", "src/apps/DelveViewer/smoke_project.json"}}}},
                buf);
            const json f = rpcCall(a, {{"op", "fill"}, {"args", json::object()}}, buf);
            aOk = l.value("ok", false) && f.value("ok", false) && f["data"].value("rooms", 0u) == 3u;
            closeFd(a);
        });
        std::thread tb([&] {
            std::string buf;
            SocketFd b = connectClient("127.0.0.1", port);
            if (b == kBadSock) return;
            const json l =
                rpcCall(b, {{"op", "load"}, {"args", {{"path", copy2.string()}}}}, buf);
            const json f = rpcCall(b, {{"op", "fill"}, {"args", json::object()}}, buf);
            bOk = l.value("ok", false) && f.value("ok", false) && f["data"].value("reran", 0u) > 0u;
            closeFd(b);
        });
        ta.join();
        tb.join();
        check(aOk && bOk, "two clients fill different slots concurrently (warm + cold)");
    }

    // 15. no_file paths
    if (cfd != kBadSock) {
        const json nf = call({{"op", "fill"}, {"args", {{"file", "no/such.json"}}}});
        check(!nf.value("ok", true) && nf["error"].value("kind", std::string{}) == "no_file",
              "rpc fill file=no/such.json -> no_file");
        std::string bufFresh;
        SocketFd fresh = connectClient("127.0.0.1", port);
        const json fv = fresh != kBadSock
                            ? rpcCall(fresh, {{"op", "validate"}, {"args", json::object()}}, bufFresh)
                            : json{};
        check(fresh != kBadSock && !fv.value("ok", true) &&
                  fv["error"].value("kind", std::string{}) == "no_file",
              "rpc validate without load on a fresh client -> no_file");
        if (fresh != kBadSock) closeFd(fresh);
    }

    // 16. cleanup
    if (cfd != kBadSock) closeFd(cfd);
    polling = false;
    poller.join();
    runtime.stopWorkers();
    server.stop();
    fs::remove_all(tmpDir, ec);

    if (g_failures == 0) {
        spdlog::info("TEST PASS: DelveServe smoke (all checks)");
    } else {
        spdlog::error("TEST FAIL: DelveServe smoke, {} check(s) failed", g_failures);
    }
    return g_failures == 0;
}
