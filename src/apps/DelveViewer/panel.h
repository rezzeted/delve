#pragma once

// DelveViewer panels (F9): the two preview panes (3D orbit + top view), the IR
// overlay over the top view, top-view picking, the selection info panel and
// the units panel. Actions that mutate viewer state (camera focus, unit
// highlight/solo) are returned as flags for main.cpp to perform.

#include <string>

#include <glm/glm.hpp>
#include <imgui.h>

#include "GeometryPreview.h"

struct Level;

// Screen rect of the last drawn preview image (window coords), needed by the
// overlay projection and picking.
struct PreviewPaneRect {
    ImVec2 rmin{0.0f, 0.0f};
    ImVec2 rmax{0.0f, 0.0f};
    bool valid = false;
};

// F9 selection: identity of the picked IR object ("" id = empty).
struct Selection {
    enum class Kind { None, Room, Wall, Node, Door } kind = Kind::None;
    std::string id;
};

// Overlay layer toggles of the top view. Defaults per plan: room contours+ids
// and doors on; wall/node owners off (too dense otherwise).
struct OverlayLayers {
    bool rooms = true;
    bool doors = true;
    bool wallOwners = false;
    bool nodeOwners = false;
    bool anchors = true;
};

struct PreviewPaneResult {
    bool clicked = false;           // non-drag LMB release on the canvas
    ImVec2 clickPos{0.0f, 0.0f};    // window coords (picking input)
};

// ImGui body of a preview pane: toolbar (Fit + summary), the target image and
// orbit/pan/zoom mouse. With overlayToggles the pane gets a second toolbar row
// of layer checkboxes (the top view).
PreviewPaneResult drawPreviewPane(GeometryPreview& preview, PreviewPaneRect& rect,
                                  const char* emptyHint, OverlayLayers* overlayToggles = nullptr);

// IR overlay over the top view's image: room contours+ids, doors, wall/node
// owners, anchors, and the bright selection pass (drawn regardless of layer
// toggles). Draws into dl (the top pane window's draw list) clipped to the
// pane's image rect.
void drawIrOverlay(ImDrawList* dl, const GeometryPreview& topPreview, const PreviewPaneRect& rect,
                   const Level& level, const OverlayLayers& layers, const Selection& selection);

// Top-view picking (the 3D view has none — documented v1 limit): the click
// point is unprojected onto the y=0 plan through the CURRENT camera (inverse
// of viewProj, so an orbited top view still picks correctly). Rooms win
// (point-in-polygon over grid x cell), then the nearest wall/door segment
// (<0.5 m, doors on ties), then the nearest node (<0.5 m).
Selection pickAtSelection(const Level& level, const GeometryPreview& topPreview,
                          const PreviewPaneRect& rect, ImVec2 clickPos);

// Selection info panel (F9 acceptance): parameters of the picked object with
// the provenance chain (delve::format_prov) of every value. The flags ask
// main.cpp to focus the camera / act on the object's fill unit.
struct InfoActions {
    bool focus = false;
    bool highlightUnit = false;
    bool soloUnit = false;
};
InfoActions drawInfoPanel(const Level& level, const Selection& selection);

// Units panel: fill units grouped by slot (collapsing headers with counts),
// Selectable rows; action buttons act on the selected unit. selectedUnit is
// in/out; highlightedUnit/soloActive only shape the button labels.
struct UnitActions {
    bool focus = false;
    bool toggleHighlight = false;
    bool toggleSolo = false;
};
UnitActions drawUnitsPanel(const Level& level, std::string& selectedUnit,
                           const std::string& highlightedUnit, bool soloActive);
