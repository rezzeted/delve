#pragma once

// TCP RPC server for DelveServe (docs/mcp_v1.md). Line-delimited JSON:
//   {"op":"<name>","args":{...}}
// answered by
//   {"ok":true,"data":{...}}  |  {"ok":false,"error":{"kind":"...","message":"..."}}
// Handlers may return nullopt to defer the reply (a CPU worker answers later
// via reply/replyError). poll() never runs pipeline work itself.
// Thread-safe: poll() is typically the main thread; workers call reply().
//
// Adapted copy of thirdparty/pgg/src/apps/PggServe/ServeRpcServer.h (same
// wire protocol and semantics; the submodule is not modified). Differences:
// the header comment, no pch include, kDefaultPort 9879 (PggServe keeps 9878).

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

class RpcException : public std::runtime_error {
public:
    std::string kind;
    std::string message;
    RpcException(std::string kind, std::string message);
};

class ServeRpcServer {
public:
    static constexpr uint16_t kDefaultPort = 9879;

    using Handler =
        std::function<std::optional<nlohmann::json>(uint64_t clientId, const nlohmann::json& args)>;

    ServeRpcServer();
    ~ServeRpcServer();

    bool start(const std::string& host = "127.0.0.1", uint16_t port = kDefaultPort);
    void stop();
    bool running() const;
    uint16_t listenPort() const;
    std::string listenHost() const { return m_host; }

    void on(const std::string& op, Handler handler);

    // Accepts, reads, dispatches complete lines, flushes writes.
    void poll();

    void reply(uint64_t clientId, const nlohmann::json& data);
    void replyError(uint64_t clientId, const std::string& kind, const std::string& message);

    [[noreturn]] static void fail(const std::string& kind, const std::string& message);

    // Per-TCP-client "current file" (canonical path). Empty until load, or
    // when the client omitted file= on a slot op.
    void setClientFile(uint64_t clientId, std::string file);
    std::string clientFile(uint64_t clientId) const;

private:
    struct Client {
        uint64_t id = 0;
        uintptr_t fd = 0;
        std::string inBuf;
        std::string outBuf;
        std::string currentFile;
    };

    void dispatch(Client& client, const std::string& line);
    void enqueue(Client& client, const nlohmann::json& response);
    void dropClientLocked(uint64_t id);

    mutable std::recursive_mutex m_mu;
    uintptr_t m_listenFd = static_cast<uintptr_t>(-1);
    bool m_running = false;
    bool m_wsaUp = false;
    uint16_t m_port = 0;
    std::string m_host;
    uint64_t m_nextClientId = 1;
    std::map<uint64_t, Client> m_clients;
    std::map<std::string, Handler> m_handlers;
};
