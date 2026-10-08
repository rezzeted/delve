#include "pch.h"

#include "topo_view.h"

#include <algorithm>
#include <cstdio>
#include <unordered_map>
#include <vector>

#include "panel.h"

namespace {

// Role palette: index = position in TopoModel::roles (deterministic).
constexpr ImU32 kRolePalette[] = {
    IM_COL32(130, 180, 250, 255),  // blue
    IM_COL32(120, 210, 140, 255),  // green
    IM_COL32(210, 130, 200, 255),  // magenta
    IM_COL32(230, 210, 110, 255),  // yellow
    IM_COL32(150, 170, 235, 255),  // indigo
    IM_COL32(120, 200, 200, 255),  // teal
    IM_COL32(235, 150, 120, 255),  // salmon
    IM_COL32(170, 200, 130, 255),  // olive
};
constexpr ImU32 kNeutral = IM_COL32(150, 150, 160, 255);
// Door colors match the top-view IR overlay (open green, other dtype orange).
constexpr ImU32 kDoorOpen = IM_COL32(90, 220, 90, 255);
constexpr ImU32 kDoorOther = IM_COL32(240, 160, 60, 255);
constexpr ImU32 kSelection = IM_COL32(255, 230, 120, 255);
constexpr ImU32 kGridMinor = IM_COL32(255, 255, 255, 12);
constexpr ImU32 kGridMajor = IM_COL32(255, 255, 255, 24);
// Graph pane (PggViewer GraphCanvas style): fixed-size node boxes in points
// (independent of zoom) and the dark canvas background.
constexpr float kGraphNodeW = 88.0f;
constexpr float kGraphNodeH = 36.0f;
constexpr ImU32 kGraphBg = IM_COL32(24, 27, 32, 255);

ImU32 withAlpha(ImU32 c, unsigned a) { return (c & 0x00FFFFFF) | (a << 24); }

ImU32 roleColor(const delve::TopoModel& model, const std::string& role) {
    const size_t n = sizeof(kRolePalette) / sizeof(kRolePalette[0]);
    for (size_t i = 0; i < model.roles.size(); ++i)
        if (model.roles[i] == role) return kRolePalette[i % n];
    return kNeutral;  // unknown/empty role (layout-only ghost)
}

ImVec2 toScreen(const TopoCam& cam, const ImVec2& origin, double x, double y) {
    return ImVec2(origin.x + cam.offsetX + static_cast<float>(x * cam.zoom),
                  origin.y + cam.offsetY + static_cast<float>(y * cam.zoom));
}

ImVec2 toWorld(const TopoCam& cam, const ImVec2& local) {
    return ImVec2((local.x - cam.offsetX) / cam.zoom, (local.y - cam.offsetY) / cam.zoom);
}

void zoomToCursor(TopoCam& cam, const ImVec2& local, float factor) {
    const ImVec2 w = toWorld(cam, local);
    cam.zoom = std::clamp(cam.zoom * factor, 2.0f, 256.0f);
    cam.offsetX = local.x - static_cast<float>(w.x * cam.zoom);
    cam.offsetY = local.y - static_cast<float>(w.y * cam.zoom);
}

void fitCamToBBox(TopoCam& cam, float w, float h, double minx, double miny, double maxx,
                  double maxy) {
    // 1.5 cells of padding around the content.
    const double spanX = std::max(maxx - minx, 1.0) + 3.0;
    const double spanY = std::max(maxy - miny, 1.0) + 3.0;
    cam.zoom = std::clamp(static_cast<float>(std::min(w / spanX, h / spanY)), 2.0f, 96.0f);
    const double midX = (minx + maxx) * 0.5;
    const double midY = (miny + maxy) * 0.5;
    cam.offsetX = static_cast<float>(w * 0.5 - midX * cam.zoom);
    cam.offsetY = static_cast<float>(h * 0.5 - midY * cam.zoom);
}

// World bbox over the placed nodes; false when nothing is placed.
bool placedBBox(const delve::TopoModel& model, double& minx, double& maxx, double& miny,
                double& maxy) {
    bool any = false;
    for (const auto& n : model.nodes) {
        if (!n.hasLayout) continue;
        if (!any) {
            minx = n.minx;
            maxx = n.maxx;
            miny = n.miny;
            maxy = n.maxy;
            any = true;
        } else {
            minx = std::min(minx, static_cast<double>(n.minx));
            maxx = std::max(maxx, static_cast<double>(n.maxx));
            miny = std::min(miny, static_cast<double>(n.miny));
            maxy = std::max(maxy, static_cast<double>(n.maxy));
        }
    }
    if (!any) minx = maxx = miny = maxy = 0;
    return any;
}

void fitPlanCam(TopoCam& cam, const delve::TopoModel& model, float w, float h) {
    double minx, maxx, miny, maxy;
    placedBBox(model, minx, maxx, miny, maxy);
    fitCamToBBox(cam, w, h, minx, miny, maxx, maxy);
}

// Fit the graph camera: like the plan, but the node boxes are fixed-size in
// points, so the world bbox is expanded by half a box (converted at an
// initial zoom) before solving the final zoom.
void fitGraphCam(TopoCam& cam, const delve::TopoModel& model, float w, float h) {
    double minx, maxx, miny, maxy;
    placedBBox(model, minx, maxx, miny, maxy);
    double spanX = std::max(maxx - minx, 1.0) + 3.0;
    double spanY = std::max(maxy - miny, 1.0) + 3.0;
    const float z0 = std::clamp(static_cast<float>(std::min(w / spanX, h / spanY)), 2.0f, 96.0f);
    spanX += 2.0 * (kGraphNodeW * 0.5 + 16.0) / z0;
    spanY += 2.0 * (kGraphNodeH * 0.5 + 16.0) / z0;
    cam.zoom = std::clamp(static_cast<float>(std::min(w / spanX, h / spanY)), 2.0f, 96.0f);
    const double midX = (minx + maxx) * 0.5;
    const double midY = (miny + maxy) * 0.5;
    cam.offsetX = static_cast<float>(w * 0.5 - midX * cam.zoom);
    cam.offsetY = static_cast<float>(h * 0.5 - midY * cam.zoom);
}

// Even-odd point-in-polygon on the XZ plan (same rule as the top overlay),
// cell coordinates.
bool ptInPolyCell(const std::vector<delve::CellPt>& c, double x, double y) {
    bool inside = false;
    const size_t n = c.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const double xi = c[i].first, yi = c[i].second;
        const double xj = c[j].first, yj = c[j].second;
        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) inside = !inside;
    }
    return inside;
}

void centerText(ImDrawList* dl, const ImVec2& at, ImU32 col, const char* text) {
    const ImVec2 ts = ImGui::CalcTextSize(text);
    dl->AddText(ImVec2(at.x - ts.x * 0.5f, at.y - ts.y * 0.5f), col, text);
}

}  // namespace

void fitTopoPlanCam(TopoPlanState& st, double minx, double miny, double maxx, double maxy) {
    fitCamToBBox(st.cam, std::max(st.viewW, 64.0f), std::max(st.viewH, 64.0f), minx, miny, maxx,
                 maxy);
}

TopoPlanResult drawTopoPlan(const delve::TopoModel& model, Selection& selection, TopoPlanState& st) {
    TopoPlanResult res;

    // The toolbar below shares the first line; remember the full content
    // width on a fresh line — GetContentRegionAvail after the row would only
    // return the part of the line still free.
    const float fullW = ImGui::GetContentRegionAvail().x;

    size_t placed = 0;
    for (const auto& n : model.nodes) placed += n.hasLayout ? 1 : 0;

    bool wantFit = false;
    if (model.nodes.empty()) {
        ImGui::TextDisabled("(no layout tier in the project)");
    } else {
        if (ImGui::SmallButton("Fit")) wantFit = true;
        ImGui::SameLine();
        char sum[96];
        std::snprintf(sum, sizeof(sum), "%zu rooms, %zu placed", model.nodes.size(), placed);
        ImGui::TextDisabled("%s", sum);
        ImGui::SameLine();
        ImGui::Checkbox("rooms", &st.layers.rooms);
        ImGui::SameLine();
        ImGui::Checkbox("ids", &st.layers.ids);
        ImGui::SameLine();
        ImGui::Checkbox("doors", &st.layers.doors);
        ImGui::SameLine();
        ImGui::Checkbox("roles", &st.layers.roles);
    }

    ImVec2 avail = ImGui::GetContentRegionAvail();
    avail.x = std::max(fullW, 64.0f);
    avail.y = std::max(avail.y, 64.0f);
    st.viewW = avail.x;
    st.viewH = avail.y;
    if (model.nodes.empty()) {
        ImGui::Dummy(avail);
        return res;
    }

    ImGui::InvisibleButton("##topo_plan_canvas", avail,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const ImVec2 rmin = ImGui::GetItemRectMin();
    const ImVec2 rmax = ImGui::GetItemRectMax();

    if (!st.fitted || wantFit) {
        fitPlanCam(st.cam, model, avail.x, avail.y);
        st.fitted = true;
    }

    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouseLocal(io.MousePos.x - rmin.x, io.MousePos.y - rmin.y);
    const ImVec2 mouseWorld = toWorld(st.cam, mouseLocal);
    const bool hovered = ImGui::IsItemHovered();

    // Zoom to cursor on the wheel, pan on any-button drag.
    if (hovered && io.MouseWheel != 0.0f)
        zoomToCursor(st.cam, mouseLocal, io.MouseWheel > 0 ? 1.2f : 1.0f / 1.2f);
    if (ImGui::IsItemActive() &&
        (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f) ||
         ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f) ||
         ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f))) {
        st.cam.offsetX += io.MouseDelta.x;
        st.cam.offsetY += io.MouseDelta.y;
    }

    // Hover: the first room whose contour contains the cursor.
    int hoverNode = -1;
    if (hovered) {
        for (size_t i = 0; i < model.nodes.size(); ++i) {
            const auto& n = model.nodes[i];
            if (n.hasLayout && ptInPolyCell(n.contour, mouseWorld.x, mouseWorld.y)) {
                hoverNode = static_cast<int>(i);
                break;
            }
        }
    }
    st.hoverNode = hoverNode;

    // Click = LMB release within a small drag threshold (as the preview panes).
    bool clicked = false;
    if (ImGui::IsItemDeactivated() && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        const ImVec2 press = io.MouseClickedPos[ImGuiMouseButton_Left];
        const float dx = io.MousePos.x - press.x, dy = io.MousePos.y - press.y;
        if (dx * dx + dy * dy < 16.0f) clicked = true;
    }
    if (clicked) {
        if (hoverNode >= 0) {
            selection = Selection{Selection::Kind::Room, model.nodes[hoverNode].id};
            res.selectionChanged = true;
        } else if (selection.kind != Selection::Kind::None) {
            selection = Selection{};
            res.selectionChanged = true;
        }
    }
    if (hoverNode >= 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
        ImGui::IsItemHovered()) {
        selection = Selection{Selection::Kind::Room, model.nodes[hoverNode].id};
        res.selectionChanged = true;
        res.focus = true;
    }

    // --- drawing ---------------------------------------------------------

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(rmin, rmax, true);

    // Cell grid over the visible world rect (dense only when it can stay sparse).
    {
        const ImVec2 w0 = toWorld(st.cam, ImVec2(0.0f, 0.0f));
        const ImVec2 w1 = toWorld(st.cam, ImVec2(avail.x, avail.y));
        const int x0 = static_cast<int>(std::floor(w0.x)), x1 = static_cast<int>(std::ceil(w1.x));
        const int y0 = static_cast<int>(std::floor(w0.y)), y1 = static_cast<int>(std::ceil(w1.y));
        if (st.cam.zoom >= 6.0f && x1 - x0 < 4000 && y1 - y0 < 4000) {
            for (int x = x0; x <= x1; ++x)
                dl->AddLine(toScreen(st.cam, rmin, x, y0), toScreen(st.cam, rmin, x, y1),
                            x % 5 == 0 ? kGridMajor : kGridMinor, 1.0f);
            for (int y = y0; y <= y1; ++y)
                dl->AddLine(toScreen(st.cam, rmin, x0, y), toScreen(st.cam, rmin, x1, y),
                            y % 5 == 0 ? kGridMajor : kGridMinor, 1.0f);
        }
    }

    // Room contours (outlines; L-shaped rooms are non-convex, fills are out
    // of scope for v1).
    if (st.layers.rooms) {
        for (size_t i = 0; i < model.nodes.size(); ++i) {
            const auto& n = model.nodes[i];
            if (!n.hasLayout) continue;
            std::vector<ImVec2> pts;
            pts.reserve(n.contour.size());
            for (const auto& p : n.contour) pts.push_back(toScreen(st.cam, rmin, p.first, p.second));
            if (pts.empty()) continue;
            const ImU32 base = st.layers.roles
                                   ? roleColor(model, n.role)
                                   : (n.corridor ? IM_COL32(150, 150, 160, 255)
                                                 : IM_COL32(130, 180, 250, 255));
            dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), withAlpha(base, 220),
                            ImDrawFlags_Closed, 1.5f);
        }
    }

    // Door segments on the shared edges (cell coords), colored by dtype.
    if (st.layers.doors) {
        for (const auto& d : model.doors) {
            const ImVec2 a = toScreen(st.cam, rmin, d.g0.first, d.g0.second);
            const ImVec2 b = toScreen(st.cam, rmin, d.g1.first, d.g1.second);
            dl->AddLine(a, b, d.door == "open" ? kDoorOpen : kDoorOther, 2.5f);
        }
    }

    // Room ids (+ role under them) at the centroids.
    if (st.layers.ids) {
        for (const auto& n : model.nodes) {
            if (!n.hasLayout) continue;
            const ImVec2 c = toScreen(st.cam, rmin, n.cx, n.cz);
            const ImU32 col = st.layers.roles ? withAlpha(roleColor(model, n.role), 235)
                                              : IM_COL32(220, 220, 230, 235);
            centerText(dl, c, col, n.id.c_str());
            if (st.layers.roles)
                centerText(dl, ImVec2(c.x, c.y + 10.0f), IM_COL32(200, 200, 210, 150),
                           n.role.c_str());
        }
    }

    // Hover follows the rooms layer; the selection pass is always bright.
    if (st.hoverNode >= 0 && st.layers.rooms) {
        const auto& n = model.nodes[st.hoverNode];
        std::vector<ImVec2> pts;
        for (const auto& p : n.contour) pts.push_back(toScreen(st.cam, rmin, p.first, p.second));
        dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), IM_COL32(255, 255, 255, 200),
                        ImDrawFlags_Closed, 2.0f);
    }
    if (selection.kind == Selection::Kind::Room) {
        for (const auto& n : model.nodes) {
            if (n.id != selection.id || !n.hasLayout) continue;
            std::vector<ImVec2> pts;
            for (const auto& p : n.contour) pts.push_back(toScreen(st.cam, rmin, p.first, p.second));
            dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), kSelection, ImDrawFlags_Closed,
                            3.0f);
        }
    }

    dl->PopClipRect();
    return res;
}

void fitTopoGraphCam(TopoGraphState& st, double minx, double miny, double maxx, double maxy) {
    fitCamToBBox(st.cam, std::max(st.viewW, 64.0f), std::max(st.viewH, 64.0f), minx, miny, maxx,
                 maxy);
}

TopoGraphResult drawTopoGraph(const delve::TopoModel& model, Selection& selection,
                              TopoGraphState& st) {
    TopoGraphResult res;

    // As in the plan pane: the toolbar shares the first line, remember the
    // full content width on a fresh line.
    const float fullW = ImGui::GetContentRegionAvail().x;

    size_t placed = 0, unplaced = 0;
    for (const auto& n : model.nodes) {
        if (n.hasLayout) placed++;
        else unplaced++;
    }

    bool wantFit = false;
    if (model.nodes.empty()) {
        ImGui::TextDisabled("(no graph in the project)");
    } else {
        if (ImGui::SmallButton("Fit")) wantFit = true;
        ImGui::SameLine();
        char sum[96];
        std::snprintf(sum, sizeof(sum), "%zu rooms, %zu passages", placed, model.edges.size());
        ImGui::TextDisabled("%s", sum);
    }

    ImVec2 avail = ImGui::GetContentRegionAvail();
    avail.x = std::max(fullW, 64.0f);
    avail.y = std::max(avail.y, 64.0f);
    // The unplaced list takes one text line below the canvas.
    if (unplaced > 0) avail.y = std::max(avail.y - ImGui::GetTextLineHeightWithSpacing(), 64.0f);
    st.viewW = avail.x;
    st.viewH = avail.y;
    if (model.nodes.empty()) {
        ImGui::Dummy(avail);
        return res;
    }

    ImGui::InvisibleButton("##topo_graph_canvas", avail,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const ImVec2 rmin = ImGui::GetItemRectMin();
    const ImVec2 rmax = ImGui::GetItemRectMax();

    if (!st.fitted || wantFit) {
        fitGraphCam(st.cam, model, avail.x, avail.y);
        st.fitted = true;
    }

    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouseLocal(io.MousePos.x - rmin.x, io.MousePos.y - rmin.y);
    const bool hovered = ImGui::IsItemHovered();

    // Zoom to cursor on the wheel, pan on any-button drag.
    if (hovered && io.MouseWheel != 0.0f)
        zoomToCursor(st.cam, mouseLocal, io.MouseWheel > 0 ? 1.2f : 1.0f / 1.2f);
    if (ImGui::IsItemActive() &&
        (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f) ||
         ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f) ||
         ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f))) {
        st.cam.offsetX += io.MouseDelta.x;
        st.cam.offsetY += io.MouseDelta.y;
    }

    // Hover: the topmost node box containing the cursor (screen-space hit in
    // canvas-local points).
    int hoverNode = -1;
    if (hovered) {
        for (int i = static_cast<int>(model.nodes.size()) - 1; i >= 0; --i) {
            const auto& n = model.nodes[i];
            if (!n.hasLayout) continue;
            const ImVec2 c = toScreen(st.cam, rmin, n.cx, n.cz);
            const float clx = c.x - rmin.x, cly = c.y - rmin.y;
            if (std::fabs(mouseLocal.x - clx) <= kGraphNodeW * 0.5f &&
                std::fabs(mouseLocal.y - cly) <= kGraphNodeH * 0.5f) {
                hoverNode = i;
                break;
            }
        }
    }
    st.hoverNode = hoverNode;

    // Click = LMB release within a small drag threshold (as the plan pane).
    bool clicked = false;
    if (ImGui::IsItemDeactivated() && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        const ImVec2 press = io.MouseClickedPos[ImGuiMouseButton_Left];
        const float dx = io.MousePos.x - press.x, dy = io.MousePos.y - press.y;
        if (dx * dx + dy * dy < 16.0f) clicked = true;
    }
    if (clicked) {
        if (hoverNode >= 0) {
            selection = Selection{Selection::Kind::Room, model.nodes[hoverNode].id};
            res.selectionChanged = true;
        } else if (selection.kind != Selection::Kind::None) {
            selection = Selection{};
            res.selectionChanged = true;
        }
    }
    if (hoverNode >= 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
        ImGui::IsItemHovered()) {
        selection = Selection{Selection::Kind::Room, model.nodes[hoverNode].id};
        res.selectionChanged = true;
        res.focus = true;
    }

    // --- drawing ---------------------------------------------------------

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(rmin, rmax, true);
    dl->AddRectFilled(rmin, rmax, kGraphBg);

    // Wires under the boxes: bezier node-center to node-center, dtype color.
    {
        std::unordered_map<std::string, int> idx;
        idx.reserve(model.nodes.size());
        for (size_t i = 0; i < model.nodes.size(); ++i)
            idx.emplace(model.nodes[i].id, static_cast<int>(i));
        for (const auto& e : model.edges) {
            const auto ia = idx.find(e.a);
            const auto ib = idx.find(e.b);
            if (ia == idx.end() || ib == idx.end()) continue;
            const auto& na = model.nodes[ia->second];
            const auto& nb = model.nodes[ib->second];
            if (!na.hasLayout || !nb.hasLayout) continue;
            const ImVec2 p0 = toScreen(st.cam, rmin, na.cx, na.cz);
            const ImVec2 p1 = toScreen(st.cam, rmin, nb.cx, nb.cz);
            const float bend = std::max(30.0f, std::fabs(p1.x - p0.x) * 0.4f);
            const ImU32 c = withAlpha(e.door == "open" ? kDoorOpen : kDoorOther, 200);
            dl->AddBezierCubic(p0, ImVec2(p0.x + bend, p0.y), ImVec2(p1.x - bend, p1.y), p1, c,
                               1.6f);
        }
    }

    // Door-type labels at the edge midpoints: a dark plate so they read over
    // the wires (only when both endpoints are placed).
    for (const auto& e : model.edges) {
        if (!e.labelOk || e.door.empty()) continue;
        const ImVec2 at = toScreen(st.cam, rmin, e.lx, e.lz);
        const ImVec2 ts = ImGui::CalcTextSize(e.door.c_str());
        const ImVec2 a(at.x - ts.x * 0.5f - 3.0f, at.y - ts.y * 0.5f - 2.0f);
        const ImVec2 b(at.x + ts.x * 0.5f + 3.0f, at.y + ts.y * 0.5f + 2.0f);
        dl->AddRectFilled(a, b, IM_COL32(16, 18, 22, 200));
        dl->AddText(ImVec2(a.x + 3.0f, a.y + 2.0f),
                    e.door == "open" ? kDoorOpen : kDoorOther, e.door.c_str());
    }

    // Node boxes: fixed 88x36 points, centered at the centroids, id + role.
    for (size_t i = 0; i < model.nodes.size(); ++i) {
        const auto& n = model.nodes[i];
        if (!n.hasLayout) continue;
        const ImVec2 c = toScreen(st.cam, rmin, n.cx, n.cz);
        const ImVec2 a(c.x - kGraphNodeW * 0.5f, c.y - kGraphNodeH * 0.5f);
        const ImVec2 b(c.x + kGraphNodeW * 0.5f, c.y + kGraphNodeH * 0.5f);
        const ImU32 role = roleColor(model, n.role);
        dl->AddRectFilled(a, b, withAlpha(role, 46), 4.0f);
        ImU32 border = withAlpha(role, 220);
        float thickness = 1.2f;
        if (selection.kind == Selection::Kind::Room && selection.id == n.id) {
            border = kSelection;
            thickness = 2.2f;
        } else if (st.hoverNode == static_cast<int>(i)) {
            border = IM_COL32(200, 210, 235, 255);
        }
        dl->AddRect(a, b, border, 4.0f, 0, thickness);
        dl->PushClipRect(a, b, true);
        dl->AddText(ImVec2(a.x + 6.0f, a.y + 4.0f), IM_COL32(232, 236, 244, 255), n.id.c_str());
        dl->AddText(ImVec2(a.x + 6.0f, a.y + 19.0f), withAlpha(role, 220), n.role.c_str());
        dl->PopClipRect();
    }

    dl->PopClipRect();

    // Unplaced graph rooms (no layout): a single text line under the canvas.
    if (unplaced > 0) {
        std::string ids;
        for (const auto& n : model.nodes)
            if (!n.hasLayout) {
                if (!ids.empty()) ids += ", ";
                ids += n.id;
            }
        ImGui::TextDisabled("unplaced: %s", ids.c_str());
    }
    return res;
}
