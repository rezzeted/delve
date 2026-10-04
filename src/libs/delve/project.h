#pragma once

// Delve project: fill tier v0 (docs/project_v0.md) + full v1 (docs/project_v1.md).
// load_project dispatches on "format" (delve-project/0 or /1).

#include <map>
#include <optional>
#include <set>
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
    // F12: keys explicitly present in THIS role entry's JSON object (a named
    // entry's own keys only — values prefilled from "*" are not explicit).
    // Drives the role steps of the provenance chains. Programmatic edits of
    // loaded entries keep exact provenance (the step value is read live), but
    // a field ADDED this way should also be inserted here, or the chain will
    // attribute it to a lower level.
    std::set<std::string> set_fields;
};

// F12 (§4.2): one resolution step of a value's provenance chain. level:
// default | project | role | template | room | side | passage. detail: level
// subject (role name or "*", template name, room id, "side_rules[i] (keys)",
// passage "a-b"); empty for default/project. value: rendered at this step.
struct ProvStep {
    std::string level, detail, value;
};
// Ascending from the lowest applicable level to the winner (last step).
using ProvChain = std::vector<ProvStep>;

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

// F6 decor rule (v2, fill.decor): up to `count` decor:<tag> units per
// matching room, each rolled against `chance`; roles empty = all rooms.
// place: "floor" (rejection-sampled spots clearing the occupied registry) or
// "wall" (sconce-style candidates along the facings). align steers floor
// spots: "any" (uniform), "center" (bbox center first), "near_door" (an
// rng-picked doorway first); both fall back to "any" sampling. radius is the
// item footprint for the occupied registry, min_dist an extra clearance on
// top of it. cut_r > 0 (D5): the placement also cuts a floor pit of that
// radius (room_fill assets declaring the optional `cuts` input; others
// silently keep a solid floor). "lamp" as a tag stays with
// lamp_step/lamp_place.
struct DecorRule {
    std::string tag;
    std::string place = "floor";       // floor | wall
    std::vector<std::string> roles;    // empty = every room
    double chance = 1.0;               // per-item probability, 0..1
    int count = 1;                     // items attempted per matching room
    double min_dist = 0.0;             // extra clearance vs occupied volumes
    std::string align = "any";         // any | center | near_door (floor only)
    double radius = 0.5;               // item footprint, meters
    double cut_r = 0.0;                // floor pit radius, meters (0 = no cut)
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
    std::string lamp_place = "ceil";  // ceil | wall (F6 lamp placement mode)
    double row_module = 0.25;
    std::map<std::string, RoleEntry> roles;  // "*" default + named roles
    TransitionDefaults transitions;
    std::vector<SideRule> side_rules;  // applied in order, later wins
    std::vector<DecorRule> decor;      // decor placement rules (v2)
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

// F12: same with per-field provenance chains (default -> "*" -> role;
// project for h when roles are empty and for wall_t without role steps).
struct RoleProvenance {
    RoleEntry entry;
    std::map<std::string, ProvChain> prov;  // h, style, floor, ceil, wall_t
};
RoleProvenance resolve_role_prov(const Project& project, const std::string& role);

// 4.2 side level (v0): facing style for a wall side. adjacent_role is ""
// for outer sides. Last matching side_rule wins; no match -> role style.
std::string resolve_side_style(const Project& project, const std::string& room_role, bool outer,
                               const std::string& adjacent_role);

// Same, over an explicitly resolved base style (v1: room -> template ->
// role -> project). resolve_side_style is this with a role-resolved base.
std::string apply_side_rules(const Project& project, const std::string& base_style, bool outer,
                             const std::string& adjacent_role);

// F12: same with the indices of the fired rules, in application order.
struct SideResolution {
    std::string style;
    std::vector<int> fired;  // indices into project.fill.side_rules
};
SideResolution apply_side_rules_prov(const Project& project, const std::string& base_style,
                                     bool outer, const std::string& adjacent_role);

// 4.2 hierarchy, v1 levels (room -> template -> role -> project).
struct ResolvedFill {
    double h = 3.0;
    double wall_t = 0.6;
    std::string style = "stone", floor = "stone", ceil = "plain";
    // F12: per-field chains (role chain, then template and room steps when
    // the corresponding FillOverride field is set). tmpl_name/room_id are
    // the step details; empty detail falls back to "template"/"room".
    std::map<std::string, ProvChain> prov;
};
ResolvedFill resolve_room_fill(const Project& project, const std::string& role,
                               const FillOverride* tmpl, const FillOverride* room);
ResolvedFill resolve_room_fill(const Project& project, const std::string& role,
                               const FillOverride* tmpl, const FillOverride* room,
                               const std::string& tmpl_name, const std::string& room_id);

// 5.6 seed split (v1): layout and fill sub-seeds. The v0 path keeps using
// Project.seed as the fill seed directly.
int layout_seed(int seed);
int fill_seed_v1(int seed);

// Diagnostic rendering (F12): the winner first, then what it overrides,
// e.g. `4 <- room "hall" <- template "grand_hall" (3.5) <- role "hall" (3)`.
std::string format_prov(const ProvChain& chain);

// F12: detail for a side-level step, e.g.
// `side_rules[1] (adjacent_role=corridor)`.
std::string side_rule_detail(const Project& project, int index);

// Name -> int code tables. Values MUST match assets codes.pgg (parity test).
// ok=false on unknown name.
int style_code(const std::string& name, bool& ok);  // stone=1 .. mortar=4 sandstone=5 none=0
int role_code(const std::string& name, bool& ok);     // hall=1 corridor=2 crypt=3 entry=4 stairs=5
int pattern_code(const std::string& name, bool& ok);  // butt=0 chase=1
int door_code(const std::string& name, bool& ok);     // open=1 gate=2
int decor_code(const std::string& name, bool& ok);    // lamp=1 drain=2
int anchor_code(const std::string& name, bool& ok);   // light=1 spawn=2 poi=3 blocker=4

}  // namespace delve
