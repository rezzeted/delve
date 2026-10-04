#include "ServeRuntime.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>

#include <spdlog/spdlog.h>

#include "catalog.h"
#include "check.h"
#include "diag.h"
#include "export.h"
#include "generate.h"

using nlohmann::json;

namespace {

namespace fs = std::filesystem;

// Fixed wording of the clientFile fallback note (docs/mcp_v1.md).
constexpr const char* kClientFileNote = "used the client current file";

double nowMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// --- args -------------------------------------------------------------------

std::string argStr(const json& args, const char* key) {
    if (!args.contains(key) || args[key].is_null()) return {};
    if (!args[key].is_string())
        ServeRpcServer::fail("bad_args", std::string("'") + key + "' must be a string");
    return args[key].get<std::string>();
}

long long argInt(const json& args, const char* key, long long dflt) {
    if (!args.contains(key) || args[key].is_null()) return dflt;
    if (!args[key].is_number())
        ServeRpcServer::fail("bad_args", std::string("'") + key + "' must be an integer");
    return args[key].get<long long>();
}

bool argBool(const json& args, const char* key, bool dflt) {
    if (!args.contains(key) || args[key].is_null()) return dflt;
    if (!args[key].is_boolean())
        ServeRpcServer::fail("bad_args", std::string("'") + key + "' must be a boolean");
    return args[key].get<bool>();
}

// Export artifact base name: the project stem; a bare "project.json" names its
// directory instead (projects/demo/project.json -> "demo"). Same rule as DelveCli.
std::string defaultExportName(const std::string& projectPath) {
    const fs::path p(projectPath);
    const std::string stem = p.stem().string();
    if (stem != "project") return stem;
    const std::string parent = p.parent_path().filename().string();
    return parent.empty() ? stem : parent;
}

bool diagsHaveErrors(const std::vector<delve::Diag>& diags) {
    for (const delve::Diag& d : diags)
        if (!d.warning) return true;
    return false;
}

}  // namespace

ServeRuntime::ServeRuntime(ServeRpcServer& rpc) : m_rpc(rpc) {}

ServeRuntime::~ServeRuntime() { stopWorkers(); }

void ServeRuntime::touchLocked(const std::string& canonical) {
    auto it = m_lruIt.find(canonical);
    if (it == m_lruIt.end()) return;
    m_lru.erase(it->second);
    m_lru.push_front(canonical);
    m_lruIt[canonical] = m_lru.begin();
}

std::shared_ptr<ProjectSession> ServeRuntime::findSlot(const std::string& canonical) const {
    std::lock_guard<std::mutex> lock(m_slotMu);
    auto it = m_slots.find(canonical);
    if (it == m_slots.end()) return nullptr;
    return it->second;
}

std::shared_ptr<ProjectSession> ServeRuntime::getOrCreateSlot(const std::string& canonical,
                                                              std::string& err) {
    std::lock_guard<std::mutex> lock(m_slotMu);
    auto it = m_slots.find(canonical);
    if (it != m_slots.end()) {
        touchLocked(canonical);
        return it->second;
    }
    if (m_slots.size() >= kMaxSlots) {
        // Evict the least recently used slot that is not mid-work right now.
        std::string victim;
        std::shared_ptr<ProjectSession> victimPtr;
        std::unique_lock<std::recursive_mutex> victimLock;
        for (auto lru = m_lru.rbegin(); lru != m_lru.rend(); ++lru) {
            auto sit = m_slots.find(*lru);
            if (sit == m_slots.end()) continue;
            std::unique_lock<std::recursive_mutex> tryLock(sit->second->mu, std::try_to_lock);
            if (tryLock.owns_lock()) {
                victim = *lru;
                victimPtr = sit->second;
                victimLock = std::move(tryLock);
                break;
            }
        }
        if (victim.empty()) {
            err = "all " + std::to_string(kMaxSlots) + " project slots are busy";
            return nullptr;
        }
        m_lru.erase(m_lruIt[victim]);
        m_lruIt.erase(victim);
        m_slots.erase(victim);
        victimLock.unlock();
        victimPtr.reset();  // destroyed outside its own mutex
    }
    auto session = std::make_shared<ProjectSession>(canonical);
    m_slots[canonical] = session;
    m_lru.push_front(canonical);
    m_lruIt[canonical] = m_lru.begin();
    return session;
}

ServeRuntime::SlotRef ServeRuntime::resolveSlot(uint64_t clientId, const json& args) {
    std::string file = argStr(args, "file");
    SlotRef ref;
    ref.usedClientFile = file.empty();
    if (file.empty()) file = m_rpc.clientFile(clientId);
    if (file.empty())
        ServeRpcServer::fail("no_file",
                             "no project file given and the client has no current file; call load first");
    const std::string canonical = canonicalServePath(file);
    ref.slot = findSlot(canonical);
    if (!ref.slot)
        ServeRpcServer::fail("no_file", "no session for '" + file + "'; call load first");
    {
        std::lock_guard<std::mutex> lock(m_slotMu);
        touchLocked(canonical);
    }
    return ref;
}

bool ServeRuntime::postCpu(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lock(m_cpuMu);
        if (m_cpuQueue.size() >= kMaxCpuQueue) return false;
        m_cpuQueue.push_back(std::move(fn));
    }
    m_cpuCv.notify_one();
    return true;
}

void ServeRuntime::defer(uint64_t clientId, std::function<json()> fn) {
    if (!postCpu([this, clientId, fn = std::move(fn)]() {
            try {
                json data = fn();
                m_rpc.reply(clientId, data);
            } catch (const RpcException& e) {
                m_rpc.replyError(clientId, e.kind, e.message);
            } catch (const std::exception& e) {
                m_rpc.replyError(clientId, "internal", e.what());
            }
        })) {
        ServeRpcServer::fail("busy", "CPU work queue is full");
    }
}

void ServeRuntime::startWorkers() {
    unsigned n = std::thread::hardware_concurrency();
    if (n < 2) n = 2;
    if (n > 4) n = 4;
    m_stop = false;
    for (unsigned i = 0; i < n; ++i) m_workers.emplace_back([this] { workerLoop(); });
}

void ServeRuntime::stopWorkers() {
    m_stop = true;
    m_cpuCv.notify_all();
    for (std::thread& t : m_workers)
        if (t.joinable()) t.join();
    m_workers.clear();
}

void ServeRuntime::workerLoop() {
    for (;;) {
        std::function<void()> fn;
        {
            std::unique_lock<std::mutex> lock(m_cpuMu);
            m_cpuCv.wait(lock, [&] { return m_stop || !m_cpuQueue.empty(); });
            if (m_stop && m_cpuQueue.empty()) return;
            fn = std::move(m_cpuQueue.front());
            m_cpuQueue.pop_front();
        }
        fn();
    }
}

json ServeRuntime::statusJson() const {
    json slots = json::array();
    {
        std::lock_guard<std::mutex> lock(m_slotMu);
        for (const std::string& path : m_lru) {
            auto it = m_slots.find(path);
            if (it == m_slots.end()) continue;
            const ProjectSession& s = *it->second;
            slots.push_back({{"file", s.canonicalPath()},
                             {"has_layout", s.has_layout},
                             {"has_ir", s.has_ir},
                             {"has_fill", s.has_fill}});
        }
    }
    return {{"slots", slots},
            {"uptime_s", startTimeSec > 0.0 ? wallNowSec() - startTimeSec : 0.0},
            {"port", m_rpc.listenPort()},
            {"assets_dir", m_assets},
            {"workers", m_workers.size()}};
}

void ServeRuntime::registerHandlers() {
    m_rpc.on("ping", [this](uint64_t, const json& args) -> std::optional<json> {
        return handlePing(args);
    });
    m_rpc.on("status", [this](uint64_t, const json& args) -> std::optional<json> {
        return handleStatus(args);
    });
    const auto deferred = [this](const char* op, json (ServeRuntime::*fn)(uint64_t, const json&)) {
        m_rpc.on(op, [this, fn](uint64_t id, const json& args) -> std::optional<json> {
            defer(id, [this, id, args, fn] { return (this->*fn)(id, args); });
            return std::nullopt;
        });
    };
    deferred("load", &ServeRuntime::handleLoad);
    deferred("validate", &ServeRuntime::handleValidate);
    deferred("layout", &ServeRuntime::handleLayout);
    deferred("ir", &ServeRuntime::handleIr);
    deferred("fill", &ServeRuntime::handleFill);
    deferred("check", &ServeRuntime::handleCheck);
    deferred("export", &ServeRuntime::handleExport);
    deferred("units", &ServeRuntime::handleUnits);
    deferred("provenance", &ServeRuntime::handleProvenance);
    deferred("asset_check", &ServeRuntime::handleAssetCheck);
}

json ServeRuntime::withSession(const SlotRef& ref, json data) const {
    data["session"] = ref.slot->sessionEcho();
    if (ref.usedClientFile) data["note"] = kClientFileNote;
    return data;
}

json ServeRuntime::handlePing(const json&) {
    return {{"pong", true}, {"app", "DelveServe"}, {"protocol", 1}};
}

json ServeRuntime::handleStatus(const json&) { return statusJson(); }

// --- pipeline steps -----------------------------------------------------------

void ServeRuntime::runLayout(const delve::Project& project, const std::string& projectPath,
                             int attempts, delve::LayoutData& out, int& seedUsed, int& attemptUsed,
                             double& ms) {
    if (!project.layout)
        ServeRpcServer::fail("no_layout",
                             projectPath + ": delve-project/0 has no layout tier; serve v1 does not "
                             "take a frozen IR — use a delve-project/1 project");
    std::string err;
    delve::layout::Catalog catalog;
    if (!delve::layout::build_catalog(project, catalog, err)) ServeRpcServer::fail("D300", err);
    delve::layout::LayoutGenerator gen;
    delve::layout::GenerateOptions opts;
    opts.attempts = attempts;
    delve::layout::LayoutResult result;
    const double t0 = nowMs();
    if (!gen.generate(project, catalog, opts, result, err)) ServeRpcServer::fail("D301", err);
    ms = nowMs() - t0;
    // The delve-layout/0 handoff roundtrip, same as DelveCli.
    std::string text;
    if (!delve::layout::write_layout_json(result, project, projectPath, text, err))
        ServeRpcServer::fail("D300", err);
    if (!delve::read_layout_json(text, out, err)) ServeRpcServer::fail("D300", err);
    seedUsed = result.seed_used;
    attemptUsed = result.attempt_used;
}

void ServeRuntime::ensureIrLocked(ProjectSession& slot) {
    if (!slot.has_layout) {
        // Auto-layout with the defaults (fill semantics): a delve-project/0
        // project gets the no_layout answer, layout-step failures keep their
        // D-codes.
        runLayout(slot.project, slot.canonicalPath(), kDefaultLayoutAttempts, slot.layoutData,
                  slot.seedUsed, slot.attemptUsed, slot.layoutMs);
        slot.has_layout = true;
    }
    if (slot.has_ir) return;
    std::string err;
    if (!delve::build_ir_from_layout(slot.layoutData, slot.project, slot.canonicalPath(), slot.ir,
                                     err))
        ServeRpcServer::fail("D200", err);
    slot.has_ir = true;
}

void ServeRuntime::ensureFillLocked(ProjectSession& slot, unsigned threads) {
    // Auto-reload the project when the file changed on disk. The layout is NOT
    // regenerated: an edit of the layout tier wants an explicit layout op.
    const std::int64_t mtime = fileMtimeNs(slot.canonicalPath());
    if (mtime != slot.projectMtimeNs) {
        delve::Project p;
        std::string err;
        if (!delve::load_project(slot.canonicalPath(), p, err)) {
            const delve::Diag d = delve::classify_project_error(err);
            ServeRpcServer::fail(d.code, d.message);
        }
        slot.project = std::move(p);
        slot.projectMtimeNs = mtime;
    }
    // The IR is cheap and always rebuilt from the kept layout (Refill semantics).
    slot.has_ir = false;
    ensureIrLocked(slot);
    if (m_assets.empty())
        ServeRpcServer::fail("D100",
                             "cannot locate the delve assets dir (no assets/codes.pgg from the cwd "
                             "or the executable upwards); pass --assets <dir>");
    delve::FillOpts opts;
    opts.delve_assets = m_assets;
    opts.threads = threads;
    opts.cache = &slot.cache;
    const double t0 = nowMs();
    std::string err;
    if (!delve::fill_level(slot.ir, slot.project, opts, slot.fill, err)) {
        const delve::Diag d = delve::classify_fill_error(err);
        ServeRpcServer::fail(d.code, d.message);
    }
    slot.fillMs = nowMs() - t0;
    slot.has_fill = true;
}

// --- ops ----------------------------------------------------------------------

json ServeRuntime::handleLoad(uint64_t clientId, const json& args) {
    const std::string path = argStr(args, "path");
    if (path.empty()) ServeRpcServer::fail("bad_args", "load needs 'path'");
    const std::string canonical = canonicalServePath(path);
    delve::Project p;
    std::string err;
    if (!delve::load_project(canonical, p, err)) {
        // Always ok:true; the diagnostics carry the D1xx/D200 classification.
        return {{"file", canonical},
                {"diagnostics", delve::diags_to_json({delve::classify_project_error(err)})},
                {"has_errors", true}};
    }
    std::string slotErr;
    auto slot = getOrCreateSlot(canonical, slotErr);
    if (!slot) ServeRpcServer::fail("busy", slotErr);
    std::lock_guard<std::recursive_mutex> lock(slot->mu);
    slot->project = std::move(p);
    slot->has_project = true;
    slot->projectMtimeNs = fileMtimeNs(canonical);
    slot->has_layout = false;
    slot->has_ir = false;
    slot->has_fill = false;
    // The F8 cache survives an explicit load: content keys invalidate on their own.
    m_rpc.setClientFile(clientId, canonical);
    return {{"file", canonical},
            {"diagnostics", json::array()},
            {"has_errors", false},
            {"session", slot->sessionEcho()}};
}

json ServeRuntime::handleValidate(uint64_t clientId, const json& args) {
    const SlotRef ref = resolveSlot(clientId, args);
    // Re-read the project from disk into a copy; the slot is never touched.
    delve::Project p;
    std::string err;
    std::vector<delve::Diag> diags;
    if (!delve::load_project(ref.slot->canonicalPath(), p, err))
        diags.push_back(delve::classify_project_error(err));
    return withSession(ref, {{"diagnostics", delve::diags_to_json(diags)},
                             {"has_errors", diagsHaveErrors(diags)}});
}

json ServeRuntime::handleLayout(uint64_t clientId, const json& args) {
    const SlotRef ref = resolveSlot(clientId, args);
    const long long attempts = argInt(args, "attempts", kDefaultLayoutAttempts);
    if (attempts < 1) ServeRpcServer::fail("bad_args", "'attempts' must be >= 1");
    std::optional<int> seed;
    if (args.contains("seed") && !args["seed"].is_null()) {
        if (!args["seed"].is_number()) ServeRpcServer::fail("bad_args", "'seed' must be an integer");
        seed = args["seed"].get<int>();
    }
    ProjectSession& slot = *ref.slot;
    std::lock_guard<std::recursive_mutex> lock(slot.mu);
    delve::Project local = slot.project;  // a seed override never touches the slot's project
    if (seed) local.seed = *seed;
    delve::LayoutData ld;
    int seedUsed = 0, attemptUsed = 0;
    double ms = 0.0;
    runLayout(local, slot.canonicalPath(), static_cast<int>(attempts), ld, seedUsed, attemptUsed, ms);
    slot.layoutData = std::move(ld);
    slot.has_layout = true;
    slot.has_ir = false;   // derived from the layout
    slot.has_fill = false; // the F8 cache stays (content keys)
    slot.seedUsed = seedUsed;
    slot.attemptUsed = attemptUsed;
    slot.layoutMs = ms;
    return withSession(ref, {{"seed_used", seedUsed},
                             {"attempt_used", attemptUsed},
                             {"ms", static_cast<long long>(ms)}});
}

json ServeRuntime::handleIr(uint64_t clientId, const json& args) {
    const SlotRef ref = resolveSlot(clientId, args);
    const std::string out = argStr(args, "out");
    ProjectSession& slot = *ref.slot;
    std::lock_guard<std::recursive_mutex> lock(slot.mu);
    ensureIrLocked(slot);
    std::string err, text;
    if (!delve::write_ir_v2_json(slot.ir, text, err)) ServeRpcServer::fail("D500", err);
    text += '\n';
    json data = {{"format", delve::kIrFormat}};
    if (!out.empty()) {
        std::ofstream f(out, std::ios::binary | std::ios::trunc);
        if (!f || !(f << text)) ServeRpcServer::fail("io_error", "cannot write " + out);
        data["wrote"] = out;
    } else {
        data["text"] = text;
    }
    return withSession(ref, data);
}

json ServeRuntime::handleFill(uint64_t clientId, const json& args) {
    const SlotRef ref = resolveSlot(clientId, args);
    const long long threads = argInt(args, "threads", 0);
    if (threads < 0) ServeRpcServer::fail("bad_args", "'threads' must be >= 0");
    ProjectSession& slot = *ref.slot;
    std::lock_guard<std::recursive_mutex> lock(slot.mu);
    ensureFillLocked(slot, static_cast<unsigned>(threads));
    const delve::FillStats& s = slot.fill.stats;
    return withSession(ref, {{"rooms", s.rooms},
                             {"bodies", s.bodies},
                             {"facings", s.facings},
                             {"nodes", s.nodes},
                             {"doors", s.doors},
                             {"lamps", s.lamps},
                             {"occupied", slot.fill.occupied.size()},
                             {"reused", s.reused.size()},
                             {"reran", s.reran.size()},
                             {"ms", static_cast<long long>(slot.fillMs)}});
}

json ServeRuntime::handleCheck(uint64_t clientId, const json& args) {
    const SlotRef ref = resolveSlot(clientId, args);
    const std::string unit = argStr(args, "unit");
    ProjectSession& slot = *ref.slot;
    std::lock_guard<std::recursive_mutex> lock(slot.mu);
    ensureFillLocked(slot, 0);
    std::vector<delve::CheckDiag> checks;
    json data;
    if (unit.empty()) {
        // B3: reused units replay their cached elements verdicts — a warm
        // re-check is seconds, not a full rescan.
        delve::check_level_cached(slot.ir, slot.project, slot.fill, &slot.cache, checks);
    } else {
        size_t matched = 0;
        delve::check_units(slot.fill, unit, checks, &matched);
        data["units"] = matched;
    }
    std::vector<delve::Diag> diags;
    for (const delve::CheckDiag& c : checks) diags.push_back(delve::make_diag("D600", c.message));
    data["errors"] = checks.size();
    data["diagnostics"] = delve::diags_to_json(diags);
    data["has_errors"] = !checks.empty();
    return withSession(ref, data);
}

json ServeRuntime::handleExport(uint64_t clientId, const json& args) {
    const SlotRef ref = resolveSlot(clientId, args);
    const std::string out = argStr(args, "out");
    if (out.empty()) ServeRpcServer::fail("bad_args", "export needs 'out'");
    const std::string name = argStr(args, "name");
    const bool splitGroups = argBool(args, "split_groups", false);
    ProjectSession& slot = *ref.slot;
    std::lock_guard<std::recursive_mutex> lock(slot.mu);
    ensureFillLocked(slot, 0);
    delve::ExportOpts opts;
    opts.dir = out;
    opts.name = name.empty() ? defaultExportName(slot.canonicalPath()) : name;
    opts.split_groups = splitGroups;
    delve::ExportResult res;
    std::string err;
    if (!delve::export_level(slot.ir, slot.fill, opts, res, err))
        ServeRpcServer::fail("io_error", err);
    return withSession(ref, {{"written", res.written}});
}

json ServeRuntime::handleUnits(uint64_t clientId, const json& args) {
    const SlotRef ref = resolveSlot(clientId, args);
    ProjectSession& slot = *ref.slot;
    std::lock_guard<std::recursive_mutex> lock(slot.mu);
    if (!slot.has_fill)
        ServeRpcServer::fail("no_fill", "no fill yet for '" + slot.canonicalPath() + "'; call fill first");
    json units = json::array();
    for (const delve::FillResult::UnitSpan& u : slot.fill.units)
        units.push_back({{"id", u.id},
                         {"slot", u.slot},
                         {"mesh", {u.meshBegin, u.meshEnd}},
                         {"anchors", {u.anchorsBegin, u.anchorsEnd}}});
    return withSession(ref, {{"units", units}});
}

json ServeRuntime::handleProvenance(uint64_t clientId, const json& args) {
    const SlotRef ref = resolveSlot(clientId, args);
    const std::string room = argStr(args, "room");
    if (room.empty()) ServeRpcServer::fail("bad_args", "provenance needs 'room'");
    const std::string key = argStr(args, "key");
    ProjectSession& slot = *ref.slot;
    std::lock_guard<std::recursive_mutex> lock(slot.mu);
    ensureIrLocked(slot);
    const delve::IrRoom* found = nullptr;
    for (const delve::IrRoom& r : slot.ir.rooms)
        if (r.id == room) {
            found = &r;
            break;
        }
    if (!found) ServeRpcServer::fail("not_found", "no room '" + room + "' in the IR");
    json entries = json::object();
    if (!key.empty()) {
        const auto it = found->prov.find(key);
        if (it == found->prov.end())
            ServeRpcServer::fail("not_found",
                                 "room '" + room + "' has no provenance key '" + key + "'");
        entries[key] = delve::format_prov(it->second);
    } else {
        for (const auto& [k, chain] : found->prov) entries[k] = delve::format_prov(chain);
    }
    return withSession(ref, {{"room", room}, {"entries", entries}});
}

json ServeRuntime::handleAssetCheck(uint64_t clientId, const json& args) {
    const SlotRef ref = resolveSlot(clientId, args);
    const std::string slotKind = argStr(args, "slot");
    const std::string asset = argStr(args, "asset");
    if (slotKind.empty() || asset.empty())
        ServeRpcServer::fail("bad_args", "asset_check needs 'slot' and 'asset'");
    ProjectSession& slot = *ref.slot;
    std::lock_guard<std::recursive_mutex> lock(slot.mu);
    std::vector<delve::Diag> diags;
    if (m_assets.empty()) {
        diags.push_back(delve::make_diag(
            "D100",
            "cannot locate the delve assets dir (no assets/codes.pgg from the cwd or the "
            "executable upwards)",
            "pass --assets <dir>"));
        return withSession(ref, {{"diagnostics", delve::diags_to_json(diags)}, {"has_errors", true}});
    }
    std::vector<std::string> roots;
    for (const std::string& r : slot.project.asset_roots)
        roots.push_back((fs::path(slot.project.dir) / r).string());
    roots.push_back(m_assets);
    // Same R-A5 tail as fill_level: slot assets may import the PGG lib.
    if (const std::string pggLib = delve::find_pgg_lib_root(m_assets); !pggLib.empty())
        roots.push_back(pggLib);
    // A relative asset resolves against the delve assets dir first, then the
    // project asset_roots.
    std::string resolved = asset;
    if (fs::path(asset).is_relative()) {
        resolved.clear();
        std::error_code ec;
        const std::string fromAssets = (fs::path(m_assets) / asset).string();
        if (fs::is_regular_file(fromAssets, ec)) resolved = fromAssets;
        for (const std::string& root : roots) {
            if (!resolved.empty()) break;
            const std::string cand = (fs::path(root) / asset).string();
            if (fs::is_regular_file(cand, ec)) resolved = cand;
        }
    }
    json data;
    if (resolved.empty()) {
        diags.push_back(delve::make_diag("D100", "asset '" + asset + "' not found under the delve "
                                                     "assets dir or the project asset_roots"));
    } else {
        std::vector<delve::SlotDiag> sds;
        delve::check_asset(slotKind, resolved, roots, sds);
        bool staticErrors = false;
        for (const delve::SlotDiag& d : sds) staticErrors = staticErrors || !d.warning;
        // A2 contract lint: one synthetic run + strict output-schema checks.
        // Only on a clean static pass (a broken interface would just make the
        // run fail with an inconclusive warning).
        if (!staticErrors) delve::lint_asset(slotKind, resolved, roots, sds);
        for (const delve::SlotDiag& d : sds) diags.push_back({d.code, d.message, {}, d.warning});
        data["asset"] = resolved;
    }
    data["diagnostics"] = delve::diags_to_json(diags);
    data["has_errors"] = diagsHaveErrors(diags);
    return withSession(ref, data);
}
