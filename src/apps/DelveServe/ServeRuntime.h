#pragma once

// DelveServe runtime (docs/mcp_v1.md): project slots (LRU, max 4), a small
// CPU worker pool and the RPC handlers of the pipeline. ping/status answer
// inline on the poll thread; every slot op is deferred — the poll thread only
// queues the job, the worker runs it under the slot mutex and answers via
// reply/replyError (RpcException kinds pass through).

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "ProjectSession.h"
#include "ServeRpcServer.h"

class ServeRuntime {
public:
    static constexpr size_t kMaxSlots = 4;
    static constexpr size_t kMaxCpuQueue = 64;
    static constexpr int kDefaultLayoutAttempts = 4;

    explicit ServeRuntime(ServeRpcServer& rpc);
    ~ServeRuntime();

    ServeRuntime(const ServeRuntime&) = delete;
    ServeRuntime& operator=(const ServeRuntime&) = delete;

    void setAssetsDir(std::string dir) { m_assets = std::move(dir); }
    const std::string& assetsDir() const { return m_assets; }

    void startWorkers();
    void stopWorkers();

    void registerHandlers();

    nlohmann::json statusJson() const;

    double startTimeSec = 0.0;

private:
    struct SlotRef {
        std::shared_ptr<ProjectSession> slot;
        bool usedClientFile = false;  // file= omitted, the client's current file was used
    };

    void defer(uint64_t clientId, std::function<nlohmann::json()> fn);
    bool postCpu(std::function<void()> fn);
    void workerLoop();

    std::shared_ptr<ProjectSession> findSlot(const std::string& canonical) const;
    std::shared_ptr<ProjectSession> getOrCreateSlot(const std::string& canonical, std::string& err);
    SlotRef resolveSlot(uint64_t clientId, const nlohmann::json& args);
    // Adds the session echo (and the clientFile fallback note) to a slot op's data.
    nlohmann::json withSession(const SlotRef& ref, nlohmann::json data) const;

    // Pipeline steps. Fail with ServeRpcServer::fail using the step's RPC kind
    // (no_layout / D-codes). The Locked ones want the caller to hold slot.mu.
    void runLayout(const delve::Project& project, const std::string& projectPath, int attempts,
                   delve::LayoutData& out, int& seedUsed, int& attemptUsed, double& ms);
    void ensureIrLocked(ProjectSession& slot);
    void ensureFillLocked(ProjectSession& slot, unsigned threads);

    nlohmann::json handlePing(const nlohmann::json& args);
    nlohmann::json handleStatus(const nlohmann::json& args);
    nlohmann::json handleLoad(uint64_t clientId, const nlohmann::json& args);
    nlohmann::json handleValidate(uint64_t clientId, const nlohmann::json& args);
    nlohmann::json handleLayout(uint64_t clientId, const nlohmann::json& args);
    nlohmann::json handleIr(uint64_t clientId, const nlohmann::json& args);
    nlohmann::json handleFill(uint64_t clientId, const nlohmann::json& args);
    nlohmann::json handleCheck(uint64_t clientId, const nlohmann::json& args);
    nlohmann::json handleExport(uint64_t clientId, const nlohmann::json& args);
    nlohmann::json handleUnits(uint64_t clientId, const nlohmann::json& args);
    nlohmann::json handleProvenance(uint64_t clientId, const nlohmann::json& args);
    nlohmann::json handleAssetCheck(uint64_t clientId, const nlohmann::json& args);

    ServeRpcServer& m_rpc;
    std::string m_assets;  // resolved once at startup (empty = fill/asset_check answer D100)
    std::atomic<bool> m_stop{false};

    mutable std::mutex m_slotMu;
    std::unordered_map<std::string, std::shared_ptr<ProjectSession>> m_slots;
    std::list<std::string> m_lru;  // front = most recently used
    std::unordered_map<std::string, std::list<std::string>::iterator> m_lruIt;
    void touchLocked(const std::string& canonical);

    std::mutex m_cpuMu;
    std::condition_variable m_cpuCv;
    std::deque<std::function<void()>> m_cpuQueue;
    std::vector<std::thread> m_workers;
};
