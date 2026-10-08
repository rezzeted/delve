// DelveViewer (F9): 3D + top preview of a filled delve level with an IR
// overlay (room ids, doors, wall/node owners, anchors), top-view picking with
// a provenance info panel, unit highlight/solo, refill/re-layout over the
// F8 unit cache, and a read-only Topo view of the level-synth topology
// (2D layout plan; the passage graph pane follows in the same tab).
//   DelveViewer [project.json] [--smoke]
// No arguments: the window opens empty; a project is opened from the side
// panel (path field + recent list) or by dragging the project .json onto the
// window. The layout is always generated from the project's layout tier
// (attempts=4); frozen IR files are a DelveCli input, not the viewer's.
// --smoke runs the same data path without a window (ctest), prints one stats
// line and exits 0. Exit codes: 0 ok, 1 data error, 2 usage error.
// v1 limits: picking only in the top view, anchors only in the overlay.

#include "pch.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <pgg/eval.h>
#include <pgg/src/eval/topology_util.h>

#include "GeometryPreview.h"
#include "filedialog.h"
#include "level.h"
#include "panel.h"
#include "topo_view.h"

#define SOKOL_IMPL
#define SOKOL_NO_ENTRY

#if !defined(SOKOL_D3D11) && !defined(SOKOL_METAL) && !defined(SOKOL_GLES3) && !defined(SOKOL_GLCORE)
    #if defined(_WIN32)
        #define SOKOL_D3D11
    #elif defined(__APPLE__)
        #define SOKOL_METAL
    #else
        #define SOKOL_GLCORE
    #endif
#endif

#include <sokol_app.h>
#include <sokol_gfx.h>
#include <sokol_glue.h>
#include <sokol_log.h>
#include <util/sokol_imgui.h>

// Xlib.h (via sokol_app.h on Linux) defines None as a macro (0L); it collides
// with Selection::Kind::None. This TU never calls Xlib directly.
#if defined(None)
    #undef None
#endif

namespace {

constexpr float kPanelWidth = 380.0f;
// Points group baked onto a copy of the level mesh for the 3D unit highlight
// (the merged mesh carries no groups of its own).
constexpr const char* kUnitGroup = "unit";

bool g_gfxOk = false;
bool g_imguiOk = false;

std::string g_argv0;
std::string g_projectArg;
std::string g_loadError;

Level g_level;

// Generate panel state (empty state and "another project" share it).
char g_projectBuf[1024] = {};
FileDialog g_projectDlg;  // session dir memory
std::vector<std::string> g_recent;  // projects, newest first
std::string g_lastOpenDir;  // parent of the last opened project (relative-path fallback)
std::string g_projectsDir;  // first-open dir of the project Browse dialog (<root>/projects)

GeometryPreview g_preview3d;
GeometryPreview g_previewTop;
PreviewPaneRect g_rect3d, g_rectTop;
OverlayLayers g_layers;
int g_activeView = 0;  // 0 = 3D (default tab), 1 = Top, 2 = Topo

// Topo tab state: the model is a pure projection of the project graph and
// the generated layout (rebuilt on load/refill/relayout), the pane state is
// the camera + layer toggles.
delve::TopoModel g_topoModel;
TopoPlanState g_topoPlan;
TopoGraphState g_topoGraph;

void rebuildTopoModel(bool resetCamera) {
    g_topoModel = g_level.project.layout
                      ? delve::build_topo(g_level.project.layout.value(), g_level.layoutData)
                      : delve::TopoModel{};
    if (resetCamera) {
        g_topoPlan = TopoPlanState{};
        g_topoGraph = TopoGraphState{};
    }
}

Selection g_selection;
std::string g_selectedUnit;     // row selected in the units panel
std::string g_highlightedUnit;  // unit with the 3D highlight group ("" = none)
std::string g_soloUnit;         // unit shown solo in the 3D pane ("" = level view)

std::vector<std::pair<std::string, bool>> g_log;  // (line, isError)

void logLine(const std::string& line, bool isError = false) {
    g_log.push_back({line, isError});
    if (g_log.size() > 12) g_log.erase(g_log.begin());
    if (isError) {
        spdlog::error("DelveViewer: {}", line);
    } else {
        spdlog::info("DelveViewer: {}", line);
    }
}

const delve::FillResult::UnitSpan* findUnit(const Level& level, const std::string& id) {
    if (id.empty()) return nullptr;
    for (const delve::FillResult::UnitSpan& u : level.fill.units)
        if (u.id == id) return &u;
    return nullptr;
}

// Fill unit of an IR selection: rooms fill as "room:<id>", walls/nodes/doors
// keep their IR ids.
std::string unitIdForSelection(const Selection& sel) {
    switch (sel.kind) {
        case Selection::Kind::Room: return "room:" + sel.id;
        case Selection::Kind::Wall:
        case Selection::Kind::Node:
        case Selection::Kind::Door: return sel.id;
        default: return {};
    }
}

// Copy of the mesh with a points group flagging [begin, end) — the 3D
// highlight renders it brighter via highlightGroup = "points:unit".
pgg::GeoPtr meshWithUnitGroup(const pgg::Geo& mesh, size_t begin, size_t end) {
    auto col = std::make_shared<pgg::BoolColumn>(mesh.pointCount(), 0);
    for (size_t i = begin; i < end && i < mesh.pointCount(); ++i) (*col)[i] = 1;
    auto set = std::make_shared<pgg::GroupSet>(mesh.pointGroups ? *mesh.pointGroups
                                                                : pgg::GroupSet{});
    set->columns[kUnitGroup] = std::move(col);
    return pgg::withGroups(mesh, pgg::Domain::Points, std::move(set));
}

// Sub-mesh of one unit span: points outside [begin, end) are dropped; the
// §8.3 cascade kills their faces and gathers the attribute columns.
pgg::GeoPtr soloSubmesh(const pgg::Geo& mesh, size_t begin, size_t end) {
    std::vector<uint8_t> drop(mesh.pointCount(), 1);
    for (size_t i = begin; i < end && i < mesh.pointCount(); ++i) drop[i] = 0;
    return pgg::topo::deleteByMask(mesh, pgg::Domain::Points, drop);
}

// 3D pane geometry: the level mesh (optionally with the highlight group) or
// the solo sub-mesh. The top pane always shows the plain level mesh.
void rebuild3d(bool refit) {
    if (!g_level.loaded || !g_level.fill.mesh) return;
    PreviewBuildOptions opts;
    pgg::GeoPtr mesh = g_level.fill.mesh;
    std::string mode;
    if (const delve::FillResult::UnitSpan* u = findUnit(g_level, g_soloUnit)) {
        mesh = soloSubmesh(*mesh, u->meshBegin, u->meshEnd);
        mode = "solo: " + g_soloUnit;
    } else if (const delve::FillResult::UnitSpan* u = findUnit(g_level, g_highlightedUnit)) {
        mesh = meshWithUnitGroup(*mesh, u->meshBegin, u->meshEnd);
        opts.highlightGroup = std::string("points:") + kUnitGroup;
        mode = "highlight: " + g_highlightedUnit;
    }
    const PreviewGeometry geo = buildPreviewGeometry(pgg::Value(std::move(mesh)), opts);
    g_preview3d.setGeometry(geo, refit);
    g_preview3d.setSummary(geo.summary + (mode.empty() ? "" : "  [" + mode + "]"));
}

void rebuildPreviews(bool refit) {
    if (!g_level.loaded || !g_level.fill.mesh) return;
    rebuild3d(refit);
    const PreviewGeometry geo =
        buildPreviewGeometry(pgg::Value(g_level.fill.mesh), PreviewBuildOptions{});
    g_previewTop.setGeometry(geo, refit);
    g_previewTop.setSummary(geo.summary + "  [fill " +
                            std::to_string(static_cast<long long>(g_level.fillMs)) + " ms]");
}

std::string fillSummary(const char* what) {
    const delve::FillStats& s = g_level.fill.stats;
    return std::string(what) + ": " + std::to_string(g_level.fill.units.size()) + " units (" +
           std::to_string(s.reused.size()) + " reused, " + std::to_string(s.reran.size()) +
           " reran), mesh " + std::to_string(g_level.fill.mesh ? g_level.fill.mesh->pointCount() : 0) +
           " pts, " + std::to_string(static_cast<long long>(g_level.fillMs)) + " ms";
}

// Selection/unit ids can move on a re-layout; reset conservatively (the ids
// survive a refill, but the spans are rebuilt anyway).
void resetViewState() {
    g_selection = Selection{};
    g_selectedUnit.clear();
    g_highlightedUnit.clear();
    g_soloUnit.clear();
}

// Session MRU of successfully opened projects, newest first.
void rememberRecent(const std::string& proj) {
    g_recent.erase(std::remove(g_recent.begin(), g_recent.end(), proj), g_recent.end());
    g_recent.insert(g_recent.begin(), proj);
    if (g_recent.size() > 8) g_recent.resize(8);
}

// Relative input paths resolve against the cwd first, then against the last
// opened project's directory.
std::string resolveInputPath(const std::string& path) {
    namespace fs = std::filesystem;
    if (path.empty() || fs::path(path).is_absolute()) return path;
    std::error_code ec;
    if (fs::is_regular_file(path, ec)) return path;
    if (!g_lastOpenDir.empty()) {
        const std::string cand = (fs::path(g_lastOpenDir) / path).string();
        if (fs::is_regular_file(cand, ec)) return cand;
    }
    return path;  // load_project will report it missing
}

std::string trimCopy(std::string s) {
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    const size_t b = s.find_first_not_of(" \t\n\r");
    if (b == std::string::npos) return {};
    size_t e = s.size();
    while (e > b && space(s[e - 1])) --e;
    return s.substr(b, e - b);
}

void openLevel(const std::string& projRaw) {
    const std::string proj = resolveInputPath(projRaw);
    std::string err;
    if (!g_level.load(proj, resolve_delve_assets(g_argv0, proj), err)) {
        g_loadError = err;
        logLine("load failed: " + err, true);
        return;
    }
    g_loadError.clear();
    resetViewState();
    rebuildTopoModel(true);  // fresh layout: fresh camera (auto-fit)
    rebuildPreviews(true);
    logLine(fillSummary("load"));
    rememberRecent(proj);
    std::snprintf(g_projectBuf, sizeof(g_projectBuf), "%s", proj.c_str());
    g_lastOpenDir = std::filesystem::path(proj).parent_path().string();
}

// Back to the empty state: the level (and its unit cache) goes away, the
// previews are cleared, the generate panel stays as it was.
void closeLevel() {
    g_level = Level{};
    g_topoModel = delve::TopoModel{};
    g_topoPlan = TopoPlanState{};
    g_topoGraph = TopoGraphState{};
    resetViewState();
    g_preview3d.clear();
    g_previewTop.clear();
    g_preview3d.setSummary({});
    g_previewTop.setSummary({});
    g_loadError.clear();
    logLine("level closed");
}

void generateFromPanel() {
    const std::string proj = trimCopy(g_projectBuf);
    if (proj.empty()) return;
    openLevel(proj);
}

void doRefill() {
    std::string err;
    if (!g_level.refill(err)) {
        logLine("refill failed: " + err, true);
        return;
    }
    resetViewState();
    rebuildTopoModel(false);  // same layout: keep the camera
    rebuildPreviews(false);  // same layout: keep the camera
    logLine(fillSummary("refill"));
}

void doRelayout() {
    std::string err;
    if (!g_level.relayout(err)) {
        logLine("re-layout failed: " + err, true);
        return;
    }
    resetViewState();
    rebuildTopoModel(true);  // new layout: fresh camera (auto-fit)
    rebuildPreviews(true);  // new layout: refit
    logLine(fillSummary("re-layout"));
}

// --- camera focus ------------------------------------------------------------

bool unitSpanBBox(const Level& level, const delve::FillResult::UnitSpan& u, glm::vec3& mn,
                  glm::vec3& mx) {
    if (!level.fill.mesh || !level.fill.mesh->positions) return false;
    const std::vector<glm::vec3>& P = *level.fill.mesh->positions;
    if (u.meshBegin >= u.meshEnd || u.meshBegin >= P.size()) return false;
    const size_t end = std::min(u.meshEnd, P.size());
    mn = glm::vec3(FLT_MAX);
    mx = glm::vec3(-FLT_MAX);
    for (size_t i = u.meshBegin; i < end; ++i) {
        mn = glm::min(mn, P[i]);
        mx = glm::max(mx, P[i]);
    }
    return true;
}

bool selectionBBox(const Level& level, const Selection& sel, glm::vec3& mn, glm::vec3& mx) {
    const double cell = level.project.fill.cell;
    mn = glm::vec3(FLT_MAX);
    mx = glm::vec3(-FLT_MAX);
    auto extend = [&](double x, double y, double z) {
        const glm::vec3 p(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
        mn = glm::min(mn, p);
        mx = glm::max(mx, p);
    };
    switch (sel.kind) {
        case Selection::Kind::Room:
            for (const delve::IrRoom& r : level.ir.rooms)
                if (r.id == sel.id) {
                    for (const delve::GridPt& g : r.grid) {
                        extend(g.first * cell, 0.0, g.second * cell);
                        extend(g.first * cell, r.h, g.second * cell);
                    }
                    return true;
                }
            return false;
        case Selection::Kind::Wall:
            for (const delve::IrWall& w : level.ir.walls)
                if (w.id == sel.id) {
                    const double h = std::max(w.h_left, w.h_right);
                    extend(w.g0.first * cell, 0.0, w.g0.second * cell);
                    extend(w.g1.first * cell, h, w.g1.second * cell);
                    return true;
                }
            return false;
        case Selection::Kind::Door:
            for (const delve::IrDoor& d : level.ir.doors)
                if (d.id == sel.id) {
                    extend(d.from.first, 0.0, d.from.second);
                    extend(d.to.first, d.h, d.to.second);
                    return true;
                }
            return false;
        case Selection::Kind::Node:
            for (const delve::IrNode& n : level.ir.nodes)
                if (n.id == sel.id) {
                    const double r = std::max(n.thick, 0.5) * 0.5;
                    extend(n.at.first * cell - r, 0.0, n.at.second * cell - r);
                    extend(n.at.first * cell + r, n.h_pillar, n.at.second * cell + r);
                    return true;
                }
            return false;
        default:
            return false;
    }
}

void focusBBox(const glm::vec3& mn, const glm::vec3& mx) {
    const glm::vec3 center = (mn + mx) * 0.5f;
    const float radius = std::max(glm::length(mx - mn) * 0.5f, 0.5f);
    g_preview3d.setTarget(center, radius);
    g_previewTop.setTarget(center, radius);
}

// --- panels ------------------------------------------------------------------

// Shared body of the generate panel: the path field with a Browse dialog, the
// session recent list, the Generate button (disabled while the project field
// is empty) and the last load error. Enter in the field generates too.
void drawGenerateControls() {
    bool generate = false;
    ImGui::SetNextItemWidth(-78.0f);
    generate |= ImGui::InputTextWithHint("##project", "project.json", g_projectBuf,
                                         sizeof(g_projectBuf), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Browse...##p")) fileDialogOpen(g_projectDlg, g_projectBuf, g_projectsDir);
    if (!g_recent.empty()) {
        if (ImGui::BeginCombo("##recent", "recent projects")) {
            for (const std::string& proj : g_recent) {
                if (ImGui::Selectable(proj.c_str()))
                    std::snprintf(g_projectBuf, sizeof(g_projectBuf), "%s", proj.c_str());
            }
            ImGui::EndCombo();
        }
    }
    const bool hasProject = !trimCopy(g_projectBuf).empty();
    if (!hasProject) ImGui::BeginDisabled();
    generate |= ImGui::Button("Generate");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Load the project, generate the layout (same seed, attempts=4), fill");
    if (!hasProject) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("or drop a project .json onto the window");
    if (generate) generateFromPanel();
    if (!g_loadError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.4f, 0.35f, 1.0f));
        ImGui::TextWrapped("%s", g_loadError.c_str());
        ImGui::PopStyleColor();
    }
}

void drawSidePanel(int h) {
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(kPanelWidth, static_cast<float>(h)), ImGuiCond_Always);
    ImGui::Begin("Delve", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);

    if (!g_level.loaded) {
        ImGui::TextDisabled("no level loaded");
        drawGenerateControls();
    } else {
        const std::string header =
            "Project: " + std::filesystem::path(g_level.projectPath).filename().string();
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        if (ImGui::CollapsingHeader(header.c_str())) {
            ImGui::TextWrapped("project: %s", g_level.projectPath.c_str());
            const delve::FillStats& s = g_level.fill.stats;
            ImGui::Text("rooms %zu  walls %zu  nodes %zu", s.rooms, s.bodies, s.nodes);
            ImGui::Text("facings %zu  doors %zu  lamps %zu", s.facings, s.doors, s.lamps);
            ImGui::Text("fill %.0f ms (%zu units: %zu reused, %zu reran)", g_level.fillMs,
                        g_level.fill.units.size(), s.reused.size(), s.reran.size());
            ImGui::Text("layout %.0f ms", g_level.layoutMs);
            ImGui::TextDisabled("unit cache: %zu outputs", g_level.unitCache.size());

            if (ImGui::Button("Refill (reload project)")) doRefill();
            ImGui::SameLine();
            if (ImGui::Button("Re-layout")) doRelayout();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Generate a fresh layout (same seed, attempts=4), rebuild the IR, refill");
            ImGui::SameLine();
            if (ImGui::Button("Close")) closeLevel();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Close the level (back to the empty state)");

            ImGui::Separator();
            ImGui::TextDisabled("another project:");
            drawGenerateControls();
        }

        if (ImGui::CollapsingHeader("Selection", ImGuiTreeNodeFlags_DefaultOpen)) {
            const InfoActions ia = drawInfoPanel(g_level, g_selection);
            const std::string selUnit = unitIdForSelection(g_selection);
            if (ia.focus) {
                glm::vec3 mn, mx;
                if (selectionBBox(g_level, g_selection, mn, mx)) focusBBox(mn, mx);
            }
            if ((ia.highlightUnit || ia.soloUnit) && !findUnit(g_level, selUnit)) {
                if (!selUnit.empty()) logLine("no fill unit for " + selUnit, true);
            } else if (ia.highlightUnit) {
                g_selectedUnit = selUnit;
                g_highlightedUnit = g_highlightedUnit == selUnit ? "" : selUnit;
                rebuild3d(false);
            } else if (ia.soloUnit) {
                g_selectedUnit = selUnit;
                g_soloUnit = selUnit;
                rebuild3d(true);  // frame the isolated piece
            }
        }

        if (ImGui::CollapsingHeader("Units", ImGuiTreeNodeFlags_DefaultOpen)) {
            const UnitActions ua =
                drawUnitsPanel(g_level, g_selectedUnit, g_highlightedUnit, !g_soloUnit.empty());
            if (ua.focus) {
                glm::vec3 mn, mx;
                if (const delve::FillResult::UnitSpan* u = findUnit(g_level, g_selectedUnit);
                    u && unitSpanBBox(g_level, *u, mn, mx))
                    focusBBox(mn, mx);
            }
            if (ua.toggleHighlight) {
                g_highlightedUnit = g_highlightedUnit == g_selectedUnit ? "" : g_selectedUnit;
                rebuild3d(false);
            }
            if (ua.toggleSolo) {
                const bool entering = g_soloUnit.empty();
                g_soloUnit = entering ? g_selectedUnit : "";
                rebuild3d(entering);  // entering solo: frame the piece; back: keep camera
            }
        }
    }

    if (ImGui::CollapsingHeader("Log", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::BeginChild("##log", ImVec2(0.0f, 110.0f), true);
        for (const auto& [text, isError] : g_log) {
            if (isError) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.4f, 0.35f, 1.0f));
                ImGui::TextWrapped("%s", text.c_str());
                ImGui::PopStyleColor();
            } else {
                ImGui::TextWrapped("%s", text.c_str());
            }
        }
        if (g_log.empty()) ImGui::TextDisabled("(empty)");
        ImGui::EndChild();
    }

    // Modal file dialog of the Browse button (drawn every frame while open;
    // the chosen path only fills the field — Generate stays a separate action).
    std::string picked;
    if (fileDialogDraw(g_projectDlg, "Open project", picked))
        std::snprintf(g_projectBuf, sizeof(g_projectBuf), "%s", picked.c_str());
    ImGui::End();
}

// Right region: one window holding the two views as tabs (3D is the default)
// instead of splitting the area; only the active tab's pane is drawn.
void drawPanes(int w, int h) {
    ImGui::SetNextWindowPos(ImVec2(kPanelWidth, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(w) - kPanelWidth, static_cast<float>(h)),
                             ImGuiCond_Always);
    ImGui::Begin("View", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
    if (ImGui::BeginTabBar("##views")) {
        if (ImGui::BeginTabItem("3D")) {
            g_activeView = 0;
            drawPreviewPane(g_preview3d, g_rect3d, "no level loaded — generate a project from the panel");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Top")) {
            g_activeView = 1;
            const PreviewPaneResult topRes =
                drawPreviewPane(g_previewTop, g_rectTop,
                                "no level loaded — generate a project from the panel", &g_layers);
            if (g_level.loaded) {
                if (topRes.clicked)
                    g_selection = pickAtSelection(g_level, g_previewTop, g_rectTop, topRes.clickPos);
                drawIrOverlay(ImGui::GetWindowDrawList(), g_previewTop, g_rectTop, g_level, g_layers,
                              g_selection);
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Topo")) {
            g_activeView = 2;
            if (g_level.loaded) {
                // Two side-by-side panes over the same TopoModel: the 2D plan
                // and the passage graph (independent cameras, half the tab
                // width each).
                const ImVec2 tabAvail = ImGui::GetContentRegionAvail();
                const float paneW = std::max((tabAvail.x - 12.0f) * 0.5f, 160.0f);
                const float paneH = std::max(tabAvail.y, 160.0f);

                TopoPlanResult planRes;
                if (ImGui::BeginChild("##topo_plan", ImVec2(paneW, paneH)))
                    planRes = drawTopoPlan(g_topoModel, g_selection, g_topoPlan);
                ImGui::EndChild();
                if (planRes.focus) {
                    for (const auto& n : g_topoModel.nodes) {
                        if (n.id == g_selection.id && n.hasLayout) {
                            fitTopoPlanCam(g_topoPlan, n.minx, n.miny, n.maxx, n.maxy);
                            break;
                        }
                    }
                }
                ImGui::SameLine();
                TopoGraphResult graphRes;
                if (ImGui::BeginChild("##topo_graph", ImVec2(paneW, paneH)))
                    graphRes = drawTopoGraph(g_topoModel, g_selection, g_topoGraph);
                ImGui::EndChild();
                if (graphRes.focus) {
                    for (const auto& n : g_topoModel.nodes) {
                        if (n.id == g_selection.id && n.hasLayout) {
                            fitTopoGraphCam(g_topoGraph, n.minx, n.miny, n.maxx, n.maxy);
                            break;
                        }
                    }
                }
            } else {
                ImGui::TextDisabled("no level loaded — generate a project from the panel");
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void init() {
    spdlog::set_level(spdlog::level::info);
    spdlog::info("DelveViewer: init()");

    sg_desc gfx = {};
    gfx.environment = sglue_environment();
    gfx.logger.func = slog_func;
    sg_setup(&gfx);
    g_gfxOk = sg_isvalid();
    if (!g_gfxOk) {
        spdlog::error("DelveViewer: sg_setup FAILED");
        return;
    }

    simgui_desc_t imguiDesc = {};
    imguiDesc.logger.func = slog_func;
    simgui_setup(&imguiDesc);
    g_imguiOk = true;

    g_preview3d.init();
    g_previewTop.init();
    g_previewTop.setProjection(PreviewProjection::OrthoTop);

    g_projectsDir = resolve_delve_projects(g_argv0);

    if (!g_projectArg.empty()) openLevel(g_projectArg);
}

void frame() {
    if (!g_gfxOk) return;

    if (g_imguiOk) {
        const float dpi = std::max(sapp_dpi_scale(), 0.01f);
        simgui_frame_desc_t fd = {};
        fd.width = sapp_width();
        fd.height = sapp_height();
        fd.delta_time = static_cast<float>(sapp_frame_duration());
        fd.dpi_scale = dpi;
        simgui_new_frame(&fd);

        // ImGui works in logical points; the framebuffer size needs the dpi.
        const int w = static_cast<int>(std::lround(sapp_widthf() / dpi));
        const int h = static_cast<int>(std::lround(sapp_heightf() / dpi));
        drawSidePanel(h);
        drawPanes(w, h);

        // Offscreen pass of the active view only: outside (before) the
        // swapchain pass that draws the ImGui image referencing its target.
        // The Topo tab draws into ImGui draw lists only — no preview pass.
        if (g_activeView == 0) {
            g_preview3d.render();
        } else if (g_activeView == 1) {
            g_previewTop.render();
        }
    }

    sg_pass_action action = {};
    action.colors[0].load_action = SG_LOADACTION_CLEAR;
    action.colors[0].clear_value = {0.10f, 0.11f, 0.13f, 1.0f};
    sg_pass pass = {};
    pass.action = action;
    pass.swapchain = sglue_swapchain();
    sg_begin_pass(&pass);
    if (g_imguiOk) simgui_render();
    sg_end_pass();
    sg_commit();
}

void cleanup() {
    if (g_imguiOk) {
        g_preview3d.shutdown();
        g_previewTop.shutdown();
        simgui_shutdown();
        g_imguiOk = false;
    }
    if (sg_isvalid()) sg_shutdown();
}

// A dropped file is an IR when its text carries the delve-ir format key
// (cheap sniff of the head; projects never mention it).
bool sniffIsIr(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string head(65536, '\0');
    in.read(head.data(), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<size_t>(in.gcount()));
    return head.find("\"delve-ir/") != std::string::npos;
}

// Drag & drop (enable_dragndrop, max 1): a project .json opens directly,
// replacing the current level. A frozen IR is not a viewer input (the layout
// is always generated from the project) — say so instead of failing later.
void handleDrop() {
    if (sapp_get_num_dropped_files() < 1) return;
    const std::string path = sapp_get_dropped_file_path(0);
    if (sniffIsIr(path)) {
        g_loadError = "frozen IR is not opened here — drop the project .json";
        logLine(g_loadError, true);
        return;
    }
    openLevel(path);
}

void event(const sapp_event* ev) {
    if (g_imguiOk) simgui_handle_event(ev);
    if (ev->type == SAPP_EVENTTYPE_FILES_DROPPED) handleDrop();
}

void printUsage() {
    std::fprintf(stderr,
                 "usage: DelveViewer [project.json] [--smoke]\n"
                 "  no arguments: open the window empty; use the Generate panel or drag & drop\n"
                 "  the project .json onto the window (the layout is always generated)\n"
                 "  --smoke      no window: generate the level, print one stats line, exit 0\n"
                 "               (needs a project; usage errors exit 2)\n"
                 "exit codes: 0 ok, 1 data error, 2 usage error\n");
}

// --smoke: the whole data path (layout -> IR -> fill with a unit cache)
// without sokol. stdout carries exactly one stats line.
int runSmoke() {
    spdlog::set_level(spdlog::level::err);
    Level level;
    std::string err;
    if (!level.load(g_projectArg, resolve_delve_assets(g_argv0, g_projectArg), err)) {
        std::fprintf(stderr, "DelveViewer --smoke: %s\n", err.c_str());
        return 1;
    }
    const delve::FillStats& s = level.fill.stats;
    std::printf("smoke ok: rooms %zu, walls %zu, facings %zu, nodes %zu, doors %zu, lamps %zu | "
                "mesh %zu pts, anchors %zu pts | units %zu (%zu reused, %zu reran) | "
                "layout %.0f ms, fill %.0f ms\n",
                s.rooms, s.bodies, s.facings, s.nodes, s.doors, s.lamps,
                level.fill.mesh ? level.fill.mesh->pointCount() : 0,
                level.fill.anchors ? level.fill.anchors->pointCount() : 0,
                level.fill.units.size(), s.reused.size(), s.reran.size(), level.layoutMs,
                level.fillMs);
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    g_argv0 = argc > 0 ? argv[0] : "";
    bool smoke = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--smoke") {
            smoke = true;
        } else if (arg.rfind("--", 0) != 0) {
            g_projectArg = arg;
        } else {
            std::fprintf(stderr, "DelveViewer: unknown option '%s'\n", arg.c_str());
            printUsage();
            return 2;
        }
    }
    if (smoke && g_projectArg.empty()) {
        std::fprintf(stderr, "DelveViewer: --smoke needs a project\n");
        printUsage();
        return 2;
    }
    if (smoke) return runSmoke();
    // GUI mode: the project is optional — no arguments opens the empty state.

    sapp_desc desc = {};
    desc.init_cb = init;
    desc.frame_cb = frame;
    desc.cleanup_cb = cleanup;
    desc.event_cb = event;
    desc.width = 1440;
    desc.height = 900;
    // Swapchain stays 1x: the MSAA lives on the preview offscreen passes.
    desc.sample_count = 1;
    desc.window_title = "DelveViewer";
    desc.high_dpi = true;
    desc.enable_dragndrop = true;
    desc.max_dropped_files = 1;  // one project .json per drop
#if defined(_WIN32)
    desc.win32.console_utf8 = true;
    desc.win32.console_attach = true;
#endif
    desc.logger.func = slog_func;
    sapp_run(&desc);
    return 0;
}
