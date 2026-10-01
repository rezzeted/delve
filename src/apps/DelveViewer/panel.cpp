#include "pch.h"

#include "panel.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <vector>

#include <sokol_app.h>
#include <sokol_gfx.h>
#include <util/sokol_imgui.h>

// Xlib.h (via sokol_app.h on Linux) defines None as a macro (0L); it collides
// with Selection::Kind::None. This TU never calls Xlib directly.
#if defined(None)
    #undef None
#endif

#include <pgg/eval.h>

#include "level.h"

namespace {

// World (x, z) on the y=0 plan plane -> pane pixels, through the same matrix
// the offscreen render used. The aspect must match the render target's
// (render() builds its mvp from targetW/targetH).
bool projectWorld(const GeometryPreview& preview, const PreviewPaneRect& rect, double x, double z,
                  ImVec2& out) {
    const float aspect = static_cast<float>(preview.targetWidth()) /
                         static_cast<float>(std::max(1, preview.targetHeight()));
    const glm::vec4 clip = preview.viewProj(aspect) * glm::vec4(static_cast<float>(x), 0.0f,
                                                                static_cast<float>(z), 1.0f);
    if (clip.w <= 1e-6f) return false;  // behind the camera (orbited away from top)
    const float nx = clip.x / clip.w;
    const float ny = clip.y / clip.w;
    // NDC y is up on every backend in use; the AddImage UV flip keyed on
    // origin_top_left presents the target identically either way, so the pane
    // mapping is uniform: right = +NDC x, down = -NDC y (OrthoTop: +X right,
    // +Z down, requirements §5.1).
    out = ImVec2(rect.rmin.x + (nx * 0.5f + 0.5f) * (rect.rmax.x - rect.rmin.x),
                 rect.rmin.y + (0.5f - ny * 0.5f) * (rect.rmax.y - rect.rmin.y));
    return true;
}

void overlayText(ImDrawList* dl, const ImVec2& at, ImU32 col, const char* text) {
    const ImVec2 ts = ImGui::CalcTextSize(text);
    dl->AddText(ImVec2(at.x - ts.x * 0.5f, at.y - ts.y * 0.5f), col, text);
}

// Even-odd point-in-polygon on the XZ plan (same rule as delve_check).
bool ptInPolyCell(const std::vector<delve::GridPt>& c, double cell, double x, double z) {
    bool inside = false;
    const size_t n = c.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const double xi = c[i].first * cell, zi = c[i].second * cell;
        const double xj = c[j].first * cell, zj = c[j].second * cell;
        if ((zi > z) != (zj > z) && x < (xj - xi) * (z - zi) / (zj - zi) + xi) inside = !inside;
    }
    return inside;
}

double distToSeg(double ax, double az, double bx, double bz, double x, double z) {
    const double dx = bx - ax, dz = bz - az;
    const double len2 = dx * dx + dz * dz;
    const double t = len2 > 0.0 ? std::clamp(((x - ax) * dx + (z - az) * dz) / len2, 0.0, 1.0) : 0.0;
    const double ex = ax + t * dx - x, ez = az + t * dz - z;
    return std::hypot(ex, ez);
}

const delve::IrRoom* findRoom(const delve::IrV2& ir, const std::string& id) {
    for (const delve::IrRoom& r : ir.rooms)
        if (r.id == id) return &r;
    return nullptr;
}

const delve::IrWall* findWall(const delve::IrV2& ir, const std::string& id) {
    for (const delve::IrWall& w : ir.walls)
        if (w.id == id) return &w;
    return nullptr;
}

const delve::IrNode* findNode(const delve::IrV2& ir, const std::string& id) {
    for (const delve::IrNode& n : ir.nodes)
        if (n.id == id) return &n;
    return nullptr;
}

const delve::IrDoor* findDoor(const delve::IrV2& ir, const std::string& id) {
    for (const delve::IrDoor& d : ir.doors)
        if (d.id == id) return &d;
    return nullptr;
}

const char* doorDtypeName(int dtype) {
    switch (dtype) {  // codes table: open=1 gate=2 (project.h door_code)
        case 1: return "open";
        case 2: return "gate";
        default: return "?";
    }
}

std::string fmtMeters(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", v);
    return buf;
}

// One row of the parameter table: label, resolved value, provenance chain.
void provRow(const std::map<std::string, delve::ProvChain>& prov, const char* key,
             const char* label, const std::string& value) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(value.c_str());
    ImGui::TableNextColumn();
    const auto it = prov.find(key);
    if (it == prov.end() || it->second.empty()) {
        ImGui::TextDisabled("-");
    } else {
        ImGui::TextWrapped("%s", delve::format_prov(it->second).c_str());
    }
}

constexpr ImGuiTableFlags kProvTableFlags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                                            ImGuiTableFlags_SizingStretchProp;

}  // namespace

PreviewPaneResult drawPreviewPane(GeometryPreview& preview, PreviewPaneRect& rect,
                                  const char* emptyHint, OverlayLayers* overlayToggles) {
    PreviewPaneResult res;
    if (ImGui::SmallButton("Fit")) preview.fit();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", preview.summary().empty() ? "(no geometry)" : preview.summary().c_str());
    if (overlayToggles) {
        ImGui::Checkbox("rooms", &overlayToggles->rooms);
        ImGui::SameLine();
        ImGui::Checkbox("doors", &overlayToggles->doors);
        ImGui::SameLine();
        ImGui::Checkbox("wall owners", &overlayToggles->wallOwners);
        ImGui::SameLine();
        ImGui::Checkbox("node owners", &overlayToggles->nodeOwners);
        ImGui::SameLine();
        ImGui::Checkbox("anchors", &overlayToggles->anchors);
    }

    ImVec2 avail = ImGui::GetContentRegionAvail();
    avail.x = std::max(avail.x, 64.0f);
    avail.y = std::max(avail.y, 64.0f);
    const ImVec2 fbScale = ImGui::GetIO().DisplayFramebufferScale;
    const int wantW = static_cast<int>(avail.x * std::max(1.0f, fbScale.x));
    const int wantH = static_cast<int>(avail.y * std::max(1.0f, fbScale.y));
    preview.ensureTarget(wantW, wantH);

    ImGui::InvisibleButton("##preview_canvas", avail,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    rect.rmin = ImGui::GetItemRectMin();
    rect.rmax = ImGui::GetItemRectMax();
    rect.valid = true;
    if (preview.texView().id != SG_INVALID_ID) {
        const bool topLeft = sg_query_features().origin_top_left;
        const ImVec2 uv0 = topLeft ? ImVec2(0, 0) : ImVec2(0, 1);
        const ImVec2 uv1 = topLeft ? ImVec2(1, 1) : ImVec2(1, 0);
        ImGui::GetWindowDrawList()->AddImage(simgui_imtextureid(preview.texView()), rect.rmin,
                                             rect.rmax, uv0, uv1);
    }
    if (!preview.hasGeometry() && emptyHint) {
        const ImVec2 ts = ImGui::CalcTextSize(emptyHint);
        ImGui::GetWindowDrawList()->AddText(
            ImVec2((rect.rmin.x + rect.rmax.x - ts.x) * 0.5f, (rect.rmin.y + rect.rmax.y - ts.y) * 0.5f),
            IM_COL32(150, 150, 160, 255), emptyHint);
    }

    const ImGuiIO& io = ImGui::GetIO();
    const bool active = ImGui::IsItemActive();
    if (ImGui::IsItemHovered() && io.MouseWheel != 0.0f) preview.nudgeDistanceWheel(io.MouseWheel);
    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f))
        preview.nudgeOrbit(io.MouseDelta.x * 0.01f, io.MouseDelta.y * 0.01f);
    if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f) ||
                   ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)))
        preview.nudgePan(io.MouseDelta.x, io.MouseDelta.y);

    // Click = LMB release within a small drag threshold (a bigger move is an
    // orbit). MouseClickedPos guards against RMB/MMB releases firing this.
    if (ImGui::IsItemDeactivated() && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        const ImVec2 press = io.MouseClickedPos[ImGuiMouseButton_Left];
        const float dx = io.MousePos.x - press.x, dy = io.MousePos.y - press.y;
        if (dx * dx + dy * dy < 16.0f) {
            res.clicked = true;
            res.clickPos = io.MousePos;
        }
    }
    return res;
}

void drawIrOverlay(ImDrawList* dl, const GeometryPreview& topPreview, const PreviewPaneRect& rect,
                   const Level& level, const OverlayLayers& layers, const Selection& selection) {
    if (!rect.valid || rect.rmax.x - rect.rmin.x < 8.0f || rect.rmax.y - rect.rmin.y < 8.0f) return;
    const double cell = level.project.fill.cell;
    auto project = [&](double x, double z, ImVec2& out) {
        return projectWorld(topPreview, rect, x, z, out);
    };

    dl->PushClipRect(rect.rmin, rect.rmax, true);

    // Room contours (grid coords x cell) + id at the centroid.
    if (layers.rooms) {
        for (const delve::IrRoom& room : level.ir.rooms) {
            std::vector<ImVec2> pts;
            pts.reserve(room.grid.size());
            double cx = 0.0, cz = 0.0;
            for (const delve::GridPt& g : room.grid) {
                ImVec2 s;
                if (!project(g.first * cell, g.second * cell, s)) continue;
                pts.push_back(s);
                cx += g.first * cell;
                cz += g.second * cell;
            }
            if (pts.empty()) continue;
            const ImU32 col = room.corridor ? IM_COL32(150, 150, 160, 220)
                                            : IM_COL32(130, 180, 250, 230);
            dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), col, ImDrawFlags_Closed, 1.5f);
            ImVec2 c;
            const double n = static_cast<double>(room.grid.size());
            if (project(cx / n, cz / n, c)) overlayText(dl, c, col, room.id.c_str());
        }
    }

    // Doors: the clear opening segment (already meters), colored by dtype.
    if (layers.doors) {
        for (const delve::IrDoor& door : level.ir.doors) {
            ImVec2 a, b;
            if (!project(door.from.first, door.from.second, a) ||
                !project(door.to.first, door.to.second, b))
                continue;
            const ImU32 col = door.dtype == 1 ? IM_COL32(90, 220, 90, 255)
                                              : IM_COL32(240, 160, 60, 255);
            dl->AddLine(a, b, col, 2.5f);
        }
    }

    // Wall owners: dim owner label at the edge midpoint (grid coords x cell).
    if (layers.wallOwners) {
        for (const delve::IrWall& wall : level.ir.walls) {
            ImVec2 s;
            if (!project((wall.g0.first + wall.g1.first) * 0.5 * cell,
                         (wall.g0.second + wall.g1.second) * 0.5 * cell, s))
                continue;
            overlayText(dl, s, IM_COL32(150, 150, 160, 190), wall.owner.c_str());
        }
    }

    // Node owners: dot + owner label at the grid vertex (x cell).
    if (layers.nodeOwners) {
        for (const delve::IrNode& node : level.ir.nodes) {
            ImVec2 s;
            if (!project(node.at.first * cell, node.at.second * cell, s)) continue;
            dl->AddCircleFilled(s, 2.5f, IM_COL32(190, 150, 230, 220));
            overlayText(dl, ImVec2(s.x, s.y - 10.0f), IM_COL32(190, 150, 230, 220),
                        node.owner.c_str());
        }
    }

    // Anchors: diamonds at the anchor plan positions; the @label
    // ("<unit>#<kind>") shows on hover.
    if (layers.anchors && level.fill.anchors && level.fill.anchors->positions) {
        using StringColumn = std::shared_ptr<const std::vector<std::string>>;
        const auto& pos = *level.fill.anchors->positions;
        const StringColumn* labels = nullptr;
        if (level.fill.anchors->pointAttrs)
            if (const pgg::AttrColumn* col = level.fill.anchors->pointAttrs->find("label"))
                labels = std::get_if<StringColumn>(&col->data);
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        for (size_t i = 0; i < pos.size(); ++i) {
            ImVec2 s;
            if (!project(pos[i].x, pos[i].z, s)) continue;
            const float r = 4.0f;
            const ImVec2 dia[4] = {{s.x, s.y - r}, {s.x + r, s.y}, {s.x, s.y + r}, {s.x - r, s.y}};
            dl->AddPolyline(dia, 4, IM_COL32(240, 220, 90, 255), ImDrawFlags_Closed, 1.5f);
            if (labels && *labels && i < (*labels)->size()) {
                const float dx = mouse.x - s.x, dy = mouse.y - s.y;
                if (dx * dx + dy * dy < 64.0f)
                    dl->AddText(ImVec2(s.x + 7.0f, s.y - 7.0f), IM_COL32(240, 220, 90, 255),
                                (**labels)[i].c_str());
            }
        }
    }

    // Selection pass: bright and thick, independent of the layer toggles.
    if (selection.kind != Selection::Kind::None) {
        const ImU32 sel = IM_COL32(255, 230, 120, 255);
        if (selection.kind == Selection::Kind::Room) {
            if (const delve::IrRoom* room = findRoom(level.ir, selection.id)) {
                std::vector<ImVec2> pts;
                for (const delve::GridPt& g : room->grid) {
                    ImVec2 s;
                    if (project(g.first * cell, g.second * cell, s)) pts.push_back(s);
                }
                if (!pts.empty())
                    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), sel,
                                    ImDrawFlags_Closed, 3.0f);
            }
        } else if (selection.kind == Selection::Kind::Wall) {
            if (const delve::IrWall* wall = findWall(level.ir, selection.id)) {
                ImVec2 a, b;
                if (project(wall->g0.first * cell, wall->g0.second * cell, a) &&
                    project(wall->g1.first * cell, wall->g1.second * cell, b))
                    dl->AddLine(a, b, sel, 3.5f);
            }
        } else if (selection.kind == Selection::Kind::Door) {
            if (const delve::IrDoor* door = findDoor(level.ir, selection.id)) {
                ImVec2 a, b;
                if (project(door->from.first, door->from.second, a) &&
                    project(door->to.first, door->to.second, b))
                    dl->AddLine(a, b, sel, 4.0f);
            }
        } else if (selection.kind == Selection::Kind::Node) {
            if (const delve::IrNode* node = findNode(level.ir, selection.id)) {
                ImVec2 s;
                if (project(node->at.first * cell, node->at.second * cell, s)) {
                    dl->AddCircleFilled(s, 4.0f, sel);
                    overlayText(dl, ImVec2(s.x, s.y - 12.0f), sel, node->owner.c_str());
                }
            }
        }
    }

    dl->PopClipRect();
}

Selection pickAtSelection(const Level& level, const GeometryPreview& topPreview,
                          const PreviewPaneRect& rect, ImVec2 clickPos) {
    Selection sel;
    if (!rect.valid || rect.rmax.x - rect.rmin.x < 1.0f) return sel;

    // Unproject the click onto the y=0 plan through the current camera: two
    // NDC depths through the inverse viewProj give the pick ray (any two
    // distinct depths in range work for both ZO and NO clip conventions).
    const float aspect = static_cast<float>(topPreview.targetWidth()) /
                         static_cast<float>(std::max(1, topPreview.targetHeight()));
    const glm::mat4 inv = glm::inverse(topPreview.viewProj(aspect));
    const float nx = (clickPos.x - rect.rmin.x) / (rect.rmax.x - rect.rmin.x) * 2.0f - 1.0f;
    const float ny = 1.0f - (clickPos.y - rect.rmin.y) / (rect.rmax.y - rect.rmin.y) * 2.0f;
    glm::vec4 a = inv * glm::vec4(nx, ny, 0.0f, 1.0f);
    glm::vec4 b = inv * glm::vec4(nx, ny, 1.0f, 1.0f);
    a /= a.w;
    b /= b.w;
    const float dy = b.y - a.y;
    if (std::abs(dy) < 1e-8f) return sel;  // ray parallel to the plan
    const float t = -a.y / dy;
    const double x = a.x + t * (b.x - a.x);
    const double z = a.z + t * (b.z - a.z);

    const double cell = level.project.fill.cell;
    for (const delve::IrRoom& room : level.ir.rooms) {
        if (ptInPolyCell(room.grid, cell, x, z)) {
            sel.kind = Selection::Kind::Room;
            sel.id = room.id;
            return sel;
        }
    }
    // Nearest wall/door segment inside 0.5 m; doors win ties.
    const delve::IrDoor* bestDoor = nullptr;
    const delve::IrWall* bestWall = nullptr;
    double doorDist = 0.5, wallDist = 0.5;
    for (const delve::IrDoor& door : level.ir.doors) {
        const double d = distToSeg(door.from.first, door.from.second, door.to.first,
                                   door.to.second, x, z);
        if (d <= doorDist) {
            doorDist = d;
            bestDoor = &door;
        }
    }
    for (const delve::IrWall& wall : level.ir.walls) {
        const double d = distToSeg(wall.g0.first * cell, wall.g0.second * cell,
                                   wall.g1.first * cell, wall.g1.second * cell, x, z);
        if (d < wallDist) {
            wallDist = d;
            bestWall = &wall;
        }
    }
    if (bestDoor && (!bestWall || doorDist <= wallDist)) {
        sel.kind = Selection::Kind::Door;
        sel.id = bestDoor->id;
        return sel;
    }
    if (bestWall) {
        sel.kind = Selection::Kind::Wall;
        sel.id = bestWall->id;
        return sel;
    }
    // Nearest node inside 0.5 m.
    const delve::IrNode* bestNode = nullptr;
    double nodeDist = 0.5;
    for (const delve::IrNode& node : level.ir.nodes) {
        const double d = std::hypot(node.at.first * cell - x, node.at.second * cell - z);
        if (d < nodeDist) {
            nodeDist = d;
            bestNode = &node;
        }
    }
    if (bestNode) {
        sel.kind = Selection::Kind::Node;
        sel.id = bestNode->id;
    }
    return sel;
}

InfoActions drawInfoPanel(const Level& level, const Selection& selection) {
    InfoActions act;
    if (!level.loaded || selection.kind == Selection::Kind::None) {
        ImGui::TextDisabled("click a room, wall, door or node in the top view");
        return act;
    }
    const delve::IrV2& ir = level.ir;

    if (selection.kind == Selection::Kind::Room) {
        const delve::IrRoom* room = findRoom(ir, selection.id);
        if (!room) return act;
        ImGui::Text("room: %s", room->id.c_str());
        ImGui::Text("role: %s%s", room->role.c_str(), room->corridor ? " (corridor)" : "");
        ImGui::Text("contour: %zu vertices", room->grid.size());
        if (ImGui::BeginTable("##room_prov", 3, kProvTableFlags)) {
            provRow(room->prov, "h", "h", fmtMeters(room->h));
            provRow(room->prov, "style", "style", room->style);
            provRow(room->prov, "floor", "floor", room->floor_style);
            provRow(room->prov, "ceil", "ceil", room->ceil_style);
            // IrRoom carries no wall_t field; the resolved value is the
            // winner of its provenance chain.
            std::string wallT = "-";
            if (const auto it = room->prov.find("wall_t");
                it != room->prov.end() && !it->second.empty())
                wallT = it->second.back().value;
            provRow(room->prov, "wall_t", "wall_t", wallT);
            ImGui::EndTable();
        }
    } else if (selection.kind == Selection::Kind::Door) {
        const delve::IrDoor* door = findDoor(ir, selection.id);
        if (!door) return act;
        ImGui::Text("door: %s", door->id.c_str());
        ImGui::Text("rooms: %s <-> %s", door->room_a.c_str(), door->room_b.c_str());
        ImGui::Text("wall: %s", door->wall.c_str());
        ImGui::Text("clear opening: %s m", fmtMeters(door->clear).c_str());
        if (ImGui::BeginTable("##door_prov", 3, kProvTableFlags)) {
            provRow(door->prov, "dtype", "dtype",
                    std::string(doorDtypeName(door->dtype)) + " (" + std::to_string(door->dtype) + ")");
            provRow(door->prov, "h", "h", fmtMeters(door->h));
            provRow(door->prov, "frame", "frame", fmtMeters(door->frame));
            provRow(door->prov, "thick", "thick", fmtMeters(door->thick));
            ImGui::EndTable();
        }
    } else if (selection.kind == Selection::Kind::Wall) {
        const delve::IrWall* wall = findWall(ir, selection.id);
        if (!wall) return act;
        ImGui::Text("wall: %s", wall->id.c_str());
        ImGui::Text("owner: %s (%s)", wall->owner.c_str(), wall->outer ? "outer" : "shared");
        ImGui::Text("sides: %s | %s", wall->room_left.empty() ? "void" : wall->room_left.c_str(),
                    wall->room_right.empty() ? "void" : wall->room_right.c_str());
        ImGui::Text("thick %s, h_left %s, h_right %s", fmtMeters(wall->thick).c_str(),
                    fmtMeters(wall->h_left).c_str(), fmtMeters(wall->h_right).c_str());
        std::string doors = wall->doors.empty() ? std::string("-") : std::string{};
        for (size_t i = 0; i < wall->doors.size(); ++i)
            doors += (i == 0 ? "" : ", ") + wall->doors[i];
        ImGui::TextWrapped("doors: %s", doors.c_str());
    } else if (selection.kind == Selection::Kind::Node) {
        const delve::IrNode* node = findNode(ir, selection.id);
        if (!node) return act;
        ImGui::Text("node: %s", node->id.c_str());
        ImGui::Text("owner: %s, thick %s, h_pillar %s", node->owner.c_str(),
                    fmtMeters(node->thick).c_str(), fmtMeters(node->h_pillar).c_str());
        ImGui::Text("faces: %zu", node->faces.size());
        ImGui::Indent();
        for (const delve::IrNodeFace& face : node->faces) {
            ImGui::BulletText("%s (%s, h %s)",
                              face.room.empty() ? "void" : face.room.c_str(), face.style.c_str(),
                              fmtMeters(face.h).c_str());
        }
        ImGui::Unindent();
    }

    if (ImGui::Button("Focus")) act.focus = true;
    ImGui::SameLine();
    if (ImGui::Button("Highlight unit")) act.highlightUnit = true;
    ImGui::SameLine();
    if (ImGui::Button("Solo unit")) act.soloUnit = true;
    return act;
}

UnitActions drawUnitsPanel(const Level& level, std::string& selectedUnit,
                           const std::string& highlightedUnit, bool soloActive) {
    UnitActions act;
    if (!level.loaded || level.fill.units.empty()) {
        ImGui::TextDisabled("(no units)");
        return act;
    }
    // Group by slot, preserving the fill (merge) order of slots and units.
    std::vector<std::string> slotOrder;
    std::map<std::string, std::vector<size_t>> bySlot;
    for (size_t i = 0; i < level.fill.units.size(); ++i) {
        const std::string& slot = level.fill.units[i].slot;
        if (bySlot.emplace(slot, std::vector<size_t>{}).second) slotOrder.push_back(slot);
        bySlot[slot].push_back(i);
    }

    ImGui::BeginChild("##units", ImVec2(0.0f, 240.0f), true);
    for (const std::string& slot : slotOrder) {
        const std::vector<size_t>& idxs = bySlot[slot];
        const std::string header = slot + " (" + std::to_string(idxs.size()) + ")";
        if (!ImGui::CollapsingHeader(header.c_str())) continue;
        ImGui::Indent();
        for (const size_t i : idxs) {
            const delve::FillResult::UnitSpan& u = level.fill.units[i];
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(u.id.c_str(), selectedUnit == u.id)) selectedUnit = u.id;
            ImGui::PopID();
        }
        ImGui::Unindent();
    }
    ImGui::EndChild();

    if (selectedUnit.empty()) return act;
    ImGui::TextWrapped("unit: %s", selectedUnit.c_str());
    if (ImGui::Button("Focus")) act.focus = true;
    ImGui::SameLine();
    if (ImGui::Button(highlightedUnit == selectedUnit ? "Unhighlight" : "Highlight"))
        act.toggleHighlight = true;
    ImGui::SameLine();
    if (ImGui::Button(soloActive ? "Back to level" : "Solo")) act.toggleSolo = true;
    return act;
}
