#pragma once

// DelveViewer Topo tab (F9 extension): read-only view of the level-synth
// topology — two side-by-side panes over the GUI-free delve::TopoModel
// (N4): the 2D layout plan (room/corridor contours and door segments in
// world grid cells, the layout tier, not meters) and the passage graph
// (node boxes + bezier wires, PggViewer GraphCanvas style). Pure ImGui
// draw lists, no new dependencies, no editing in v1.

#include <imgui.h>

#include "topo.h"

struct Selection;  // panel.h

// Plan pane camera, pane-local points: screen = offset + world * zoom, where
// world is in world grid cells (1 unit = one edgar cell).
struct TopoCam {
    float offsetX = 60.0f;
    float offsetY = 60.0f;
    float zoom = 24.0f;  // points per cell
};

// Layer toggles of the plan pane (defaults: everything on).
struct PlanLayers {
    bool rooms = true;
    bool ids = true;
    bool doors = true;
    bool roles = true;
};

struct TopoPlanState {
    TopoCam cam;
    PlanLayers layers;
    int hoverNode = -1;  // hovered room index in the model (-1 = none)
    bool fitted = false; // first-frame auto-fit already done
    float viewW = 0.0f;  // last canvas size in points (for fitTopoPlanCam)
    float viewH = 0.0f;
};

// Actions for main.cpp (the result-flag pattern of panel.h).
struct TopoPlanResult {
    bool selectionChanged = false;
    bool focus = false;  // double-click on a room: focus the cameras on it
};

// Plan pane body: a toolbar row (Fit + summary + layer checkboxes) and the
// canvas with pan (LMB/RMB/MMB drag), zoom to cursor (wheel) and picking —
// a click on a room selects it (Selection::Kind::Room), a click on empty
// space clears the selection, a double-click also asks to focus.
TopoPlanResult drawTopoPlan(const delve::TopoModel& model, Selection& selection, TopoPlanState& st);

// Fit the plan camera to a bbox in world grid cells (double-click focus).
void fitTopoPlanCam(TopoPlanState& st, double minx, double miny, double maxx, double maxy);

// Graph pane state: the passage graph in the same world-cell space as the
// plan (node boxes at room centroids), independent camera.
struct TopoGraphState {
    TopoCam cam;
    int hoverNode = -1;  // hovered node index in the model (-1 = none)
    bool fitted = false;
    float viewW = 0.0f;  // last canvas size in points (for fitTopoGraphCam)
    float viewH = 0.0f;
};

// Actions for main.cpp (the result-flag pattern of panel.h).
struct TopoGraphResult {
    bool selectionChanged = false;
    bool focus = false;  // double-click a node: focus the graph camera on it
};

// Graph pane body: a toolbar row (Fit + summary), the node/wire canvas with
// pan (LMB/RMB/MMB drag), zoom to cursor (wheel) and picking — a click on a
// node selects the room, a click on empty space clears the selection, a
// double-click also asks to focus. Unplaced graph rooms (no layout) are not
// drawn; they are listed as text under the canvas.
TopoGraphResult drawTopoGraph(const delve::TopoModel& model, Selection& selection, TopoGraphState& st);

// Fit the graph camera to a bbox in world grid cells (double-click focus).
void fitTopoGraphCam(TopoGraphState& st, double minx, double miny, double maxx, double maxy);
