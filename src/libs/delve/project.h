#pragma once

// Delve project: fill tier v0 (docs/project_v0.md) + full v1 (docs/project_v1.md).
// load_project dispatches on "format" (delve-project/0 or /1).

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "layout.h"

namespace delve {

inline constexpr const char* kProjectFormat = "delve-project/0";

struct RoleEntry {
    double h = 3.0;
    std::string style = "stone";
    std::string floor = "stone";
    std::string ceil = "plain";
    std::optional<double> wall_t;  // v1 only (role level); nullopt = project wall_t
};

struct TransitionDefaults {
    std::string pattern = "butt";
    double width = 1.0;
    std::string place = "corner";  // corner | wall
};

// 4.2 side level, v0 subset: style-only rules over wall sides. All present
// match keys must hold; later rules win over earlier ones; no match falls
// back to the room's role style. Outer sides never match adjacent_role.
struct SideRule {
    std::string side;           // "" (any) | "outer" | "shared"
    std::string adjacent_role;  // "" (any) | role name
    std::string style;
};

struct FillParams {
    double cell = 2.0;
    double wall_t = 0.6;
    double min_passage = 1.2;
    double min_opening = 0.8;
    double room_h = 3.0;
    double door_h = 2.2;
    double frame = 0.15;
    double lamp_step = 4.0;
    double row_module = 0.25;
    std::map<std::string, RoleEntry> roles;  // "*" default + named roles
    TransitionDefaults transitions;
    std::vector<SideRule> side_rules;  // applied in order, later wins
};

struct Project {
    std::string format = kProjectFormat;
    int seed = 1;
    FillParams fill;
    std::optional<LayoutParams> layout;  // v1 only; nullopt on /0
    std::map<std::string, std::string> slots;  // slot kind -> asset path
    std::vector<std::string> asset_roots;
    std::string dir;  // project file's directory (resolves relative asset_roots)
};

// Strict load (R-P2): unknown keys, bad types and 5.4 violations are errors
// naming key/expectation/fact. Role names are validated against the fixed v0 set.
bool load_project(const std::string& path, Project& out, std::string& err);

// Role of an IR-0 room (v0 mapping; real roles come from the graph at D2).
std::string room_role(bool corridor);

// 4.2 hierarchy, v0 levels (project -> role). Returns the winning entry.
RoleEntry resolve_role(const Project& project, const std::string& role);

// 4.2 side level (v0): facing style for a wall side. adjacent_role is ""
// for outer sides. Last matching side_rule wins; no match -> role style.
std::string resolve_side_style(const Project& project, const std::string& room_role, bool outer,
                               const std::string& adjacent_role);

// Same, over an explicitly resolved base style (v1: room -> template ->
// role -> project). resolve_side_style is this with a role-resolved base.
std::string apply_side_rules(const Project& project, const std::string& base_style, bool outer,
                             const std::string& adjacent_role);

// 4.2 hierarchy, v1 levels (room -> template -> role -> project).
struct ResolvedFill {
    double h = 3.0;
    double wall_t = 0.6;
    std::string style = "stone", floor = "stone", ceil = "plain";
};
ResolvedFill resolve_room_fill(const Project& project, const std::string& role,
                               const FillOverride* tmpl, const FillOverride* room);

// 5.6 seed split (v1): layout and fill sub-seeds. The v0 path keeps using
// Project.seed as the fill seed directly.
int layout_seed(int seed);
int fill_seed_v1(int seed);

// Name -> int code tables. Values MUST match assets codes.pgg (parity test).
// ok=false on unknown name.
int style_code(const std::string& name, bool& ok);    // stone=1 brick=2 plain=3 mortar=4
int role_code(const std::string& name, bool& ok);     // hall=1 corridor=2 crypt=3 entry=4 stairs=5
int pattern_code(const std::string& name, bool& ok);  // butt=0 chase=1
int door_code(const std::string& name, bool& ok);     // open=1 gate=2
int decor_code(const std::string& name, bool& ok);    // lamp=1
int anchor_code(const std::string& name, bool& ok);   // light=1 spawn=2 poi=3

}  // namespace delve
