// Adapted copy of thirdparty/pgg/src/apps/PggServe/ServeRpcServer.cpp for
// DelveServe (docs/mcp_v1.md); the submodule is not modified. Platform socket
// code (winsock/posix) is kept as is; the only edit is the dropped pch include.

#include "ServeRpcServer.h"

#include <cstring>

#include <spdlog/spdlog.h>

#if defined(_WIN32)
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
using SockLen = int;
static constexpr uintptr_t kInvalidFd = static_cast<uintptr_t>(INVALID_SOCKET);
static int closeSocket(uintptr_t fd) { return closesocket(static_cast<SOCKET>(fd)); }
static int sockError() { return WSAGetLastError(); }
static bool sockWouldBlock(int err) { return err == WSAEWOULDBLOCK; }
#else
    #include <arpa/inet.h>
    #include <cerrno>
    #include <fcntl.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <sys/socket.h>
    #include <unistd.h>
using SockLen = socklen_t;
static constexpr uintptr_t kInvalidFd = static_cast<uintptr_t>(-1);
static int closeSocket(uintptr_t fd) { return ::close(static_cast<int>(fd)); }
static int sockError() { return errno; }
static bool sockWouldBlock(int err) { return err == EAGAIN || err == EWOULDBLOCK; }
#endif

namespace {

bool setNonBlocking(uintptr_t fd) {
#if defined(_WIN32)
    u_long mode = 1;
    return ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(static_cast<int>(fd), F_GETFL, 0);
    return flags >= 0 && fcntl(static_cast<int>(fd), F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

void setNoSigpipe(uintptr_t fd) {
#if defined(SO_NOSIGPIPE)
    const int one = 1;
    setsockopt(static_cast<int>(fd), SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

int sendFlags() {
#if defined(MSG_NOSIGNAL)
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

ptrdiff_t sockSend(uintptr_t fd, const char* data, size_t size) {
#if defined(_WIN32)
    return static_cast<ptrdiff_t>(
        ::send(static_cast<SOCKET>(fd), data, static_cast<int>(size), sendFlags()));
#else
    return ::send(static_cast<int>(fd), data, size, sendFlags());
#endif
}

ptrdiff_t sockRecv(uintptr_t fd, char* data, size_t size) {
#if defined(_WIN32)
    return static_cast<ptrdiff_t>(
        ::recv(static_cast<SOCKET>(fd), data, static_cast<int>(size), 0));
#else
    return ::recv(static_cast<int>(fd), data, size, 0);
#endif
}

}  // namespace

RpcException::RpcException(std::string kind, std::string message)
    : std::runtime_error(kind + ": " + message), kind(std::move(kind)), message(std::move(message)) {}

ServeRpcServer::ServeRpcServer() = default;

ServeRpcServer::~ServeRpcServer() { stop(); }

bool ServeRpcServer::running() const {
    std::lock_guard<std::recursive_mutex> lock(m_mu);
    return m_running;
}

uint16_t ServeRpcServer::listenPort() const {
    std::lock_guard<std::recursive_mutex> lock(m_mu);
    return m_port;
}

bool ServeRpcServer::start(const std::string& host, uint16_t port) {
    stop();
#if defined(_WIN32)
    WSADATA wsa = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        spdlog::error("ServeRpcServer: WSAStartup failed");
        return false;
    }
    m_wsaUp = true;
#endif
    const int fd = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
    if (fd < 0) {
        spdlog::error("ServeRpcServer: socket() failed: {}", sockError());
        return false;
    }
    m_listenFd = static_cast<uintptr_t>(fd);
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
    setNoSigpipe(m_listenFd);

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        spdlog::error("ServeRpcServer: bad listen host '{}'", host);
        stop();
        return false;
    }
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(fd, 32) != 0) {
        spdlog::error("ServeRpcServer: cannot listen on {}:{} ({})", host, port, sockError());
        stop();
        return false;
    }
    if (!setNonBlocking(m_listenFd)) {
        spdlog::error("ServeRpcServer: cannot make the listen socket non-blocking");
        stop();
        return false;
    }
    sockaddr_in bound = {};
    SockLen boundLen = sizeof(bound);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &boundLen) == 0)
        m_port = ntohs(bound.sin_port);
    else
        m_port = port;
    m_host = host;
    m_running = true;
    spdlog::info("ServeRpcServer: listening on {}:{}", host, m_port);
    return true;
}

void ServeRpcServer::stop() {
    std::lock_guard<std::recursive_mutex> lock(m_mu);
    for (auto& [id, client] : m_clients) closeSocket(client.fd);
    m_clients.clear();
    if (m_running || m_listenFd != kInvalidFd) {
        if (m_listenFd != kInvalidFd) closeSocket(m_listenFd);
        m_listenFd = kInvalidFd;
        m_running = false;
        m_port = 0;
    }
#if defined(_WIN32)
    if (m_wsaUp) {
        WSACleanup();
        m_wsaUp = false;
    }
#endif
}

void ServeRpcServer::on(const std::string& op, Handler handler) {
    std::lock_guard<std::recursive_mutex> lock(m_mu);
    m_handlers[op] = std::move(handler);
}

void ServeRpcServer::fail(const std::string& kind, const std::string& message) {
    throw RpcException(kind, message);
}

void ServeRpcServer::enqueue(Client& client, const nlohmann::json& response) {
    client.outBuf += response.dump();
    client.outBuf += '\n';
    while (!client.outBuf.empty()) {
        const ptrdiff_t n = sockSend(client.fd, client.outBuf.data(), client.outBuf.size());
        if (n > 0) {
            client.outBuf.erase(0, static_cast<size_t>(n));
        } else if (n < 0 && !sockWouldBlock(sockError())) {
            dropClientLocked(client.id);
            return;
        } else {
            break;
        }
    }
}

void ServeRpcServer::dropClientLocked(uint64_t id) {
    auto it = m_clients.find(id);
    if (it == m_clients.end()) return;
    closeSocket(it->second.fd);
    m_clients.erase(it);
}

void ServeRpcServer::dispatch(Client& client, const std::string& line) {
    nlohmann::json req = nlohmann::json::parse(line, nullptr, false);
    if (req.is_discarded() || !req.is_object()) {
        enqueue(client, {{"ok", false},
                         {"error", {{"kind", "bad_json"}, {"message", "request is not a JSON object"}}}});
        return;
    }
    const std::string op = req.value("op", std::string{});
    const nlohmann::json args = req.value("args", nlohmann::json::object());
    const auto it = m_handlers.find(op);
    if (it == m_handlers.end()) {
        enqueue(client, {{"ok", false},
                         {"error", {{"kind", "unknown_op"}, {"message", "unknown op '" + op + "'"}}}});
        return;
    }
    try {
        std::optional<nlohmann::json> data = it->second(client.id, args);
        if (data) enqueue(client, {{"ok", true}, {"data", *data}});
    } catch (const RpcException& e) {
        enqueue(client, {{"ok", false}, {"error", {{"kind", e.kind}, {"message", e.message}}}});
    } catch (const std::exception& e) {
        enqueue(client, {{"ok", false}, {"error", {{"kind", "internal"}, {"message", e.what()}}}});
    }
}

void ServeRpcServer::reply(uint64_t clientId, const nlohmann::json& data) {
    std::lock_guard<std::recursive_mutex> lock(m_mu);
    auto it = m_clients.find(clientId);
    if (it == m_clients.end()) return;
    enqueue(it->second, {{"ok", true}, {"data", data}});
}

void ServeRpcServer::replyError(uint64_t clientId, const std::string& kind, const std::string& message) {
    std::lock_guard<std::recursive_mutex> lock(m_mu);
    auto it = m_clients.find(clientId);
    if (it == m_clients.end()) return;
    enqueue(it->second, {{"ok", false}, {"error", {{"kind", kind}, {"message", message}}}});
}

void ServeRpcServer::setClientFile(uint64_t clientId, std::string file) {
    std::lock_guard<std::recursive_mutex> lock(m_mu);
    auto it = m_clients.find(clientId);
    if (it == m_clients.end()) return;
    it->second.currentFile = std::move(file);
}

std::string ServeRpcServer::clientFile(uint64_t clientId) const {
    std::lock_guard<std::recursive_mutex> lock(m_mu);
    auto it = m_clients.find(clientId);
    if (it == m_clients.end()) return {};
    return it->second.currentFile;
}

void ServeRpcServer::poll() {
    std::lock_guard<std::recursive_mutex> lock(m_mu);
    if (!m_running) return;

    for (;;) {
        sockaddr_in addr = {};
        SockLen addrLen = sizeof(addr);
#if defined(_WIN32)
        const SOCKET c =
            accept(static_cast<SOCKET>(m_listenFd), reinterpret_cast<sockaddr*>(&addr), &addrLen);
        if (c == INVALID_SOCKET) break;
        const uintptr_t cfd = static_cast<uintptr_t>(c);
#else
        const int c = accept(static_cast<int>(m_listenFd), reinterpret_cast<sockaddr*>(&addr), &addrLen);
        if (c < 0) break;
        const uintptr_t cfd = static_cast<uintptr_t>(c);
#endif
        setNonBlocking(cfd);
        setNoSigpipe(cfd);
        const int one = 1;
        setsockopt(static_cast<int>(cfd), IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&one), sizeof(one));
        Client client;
        client.id = m_nextClientId++;
        client.fd = cfd;
        m_clients.emplace(client.id, std::move(client));
    }

    std::vector<uint64_t> dead;
    for (auto& [id, client] : m_clients) {
        size_t received = 0;
        for (;;) {
            char buf[16384];
            const ptrdiff_t n = sockRecv(client.fd, buf, sizeof(buf));
            if (n > 0) {
                client.inBuf.append(buf, static_cast<size_t>(n));
                received += static_cast<size_t>(n);
                if (received >= (1u << 20)) break;
            } else if (n == 0) {
                dead.push_back(id);
                break;
            } else {
                if (!sockWouldBlock(sockError())) dead.push_back(id);
                break;
            }
        }
        for (;;) {
            const size_t nl = client.inBuf.find('\n');
            if (nl == std::string::npos) break;
            std::string line = client.inBuf.substr(0, nl);
            client.inBuf.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            dispatch(client, line);
            if (m_clients.find(id) == m_clients.end()) break;
        }
        while (m_clients.find(id) != m_clients.end() && !client.outBuf.empty()) {
            const ptrdiff_t n = sockSend(client.fd, client.outBuf.data(), client.outBuf.size());
            if (n > 0) {
                client.outBuf.erase(0, static_cast<size_t>(n));
            } else if (n < 0 && !sockWouldBlock(sockError())) {
                dead.push_back(id);
                break;
            } else {
                break;
            }
        }
    }
    for (const uint64_t id : dead) dropClientLocked(id);
}
