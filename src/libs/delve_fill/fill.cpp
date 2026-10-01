// Delve F6: IR -> units -> PGG runs -> assembled level (R-A9 local frames).

#include "fill.h"

#include <cmath>
#include <filesystem>
#include <map>
#include <numbers>

#include <glm/glm.hpp>

#include "pgg/src/eval/geometry.h"
#include "pgg/src/eval/modules.h"

namespace delve {
namespace {

constexpr double kEps = 1e-9;

double yawOf(double dx, double dz) { return std::atan2(-dz, dx) * 180.0 / std::numbers::pi; }

// --- points payloads -------------------------------------------------------

struct PointsBuilder {
    std::vector<glm::vec3> pos;
    std::map<std::string, std::vector<float>> f32c;
    std::map<std::string, std::vector<int64_t>> intc;
    std::map<std::string, std::vector<uint8_t>> boolc;
    std::map<std::string, std::vector<glm::vec3>> vec3c;

    void pt(double x, double y, double z) { pos.emplace_back((float)x, (float)y, (float)z); }
    void f32(const std::string& name, double v) { f32c[name].push_back((float)v); }
    void integer(const std::string& name, int64_t v) { intc[name].push_back(v); }
    void boolean(const std::string& name, bool v) { boolc[name].push_back(v ? 1 : 0); }
    void vec3(const std::string& name, double x, double y, double z) {
        vec3c[name].emplace_back((float)x, (float)y, (float)z);
    }
    pgg::GeoPtr build(std::string& err) const {
        auto bad = [&](const std::string& name, size_t n) {
            err = "internal: column '" + name + "' has " + std::to_string(n) + " values for " +
                  std::to_string(pos.size()) + " points";
            return pgg::GeoPtr{};
        };
        auto attrs = std::make_shared<pgg::AttrSet>();
        for (const auto& [name, col] : f32c) {
            if (col.size() != pos.size()) return bad(name, col.size());
            attrs->columns[name] =
                pgg::AttrColumn{std::make_shared<const std::vector<float>>(col)};
        }
        for (const auto& [name, col] : intc) {
            if (col.size() != pos.size()) return bad(name, col.size());
            attrs->columns[name] =
                pgg::AttrColumn{std::make_shared<const std::vector<int64_t>>(col)};
        }
        for (const auto& [name, col] : boolc) {
            if (col.size() != pos.size()) return bad(name, col.size());
            attrs->columns[name] =
                pgg::AttrColumn{std::make_shared<const std::vector<uint8_t>>(col)};
        }
        for (const auto& [name, col] : vec3c) {
            if (col.size() != pos.size()) return bad(name, col.size());
            attrs->columns[name] =
                pgg::AttrColumn{std::make_shared<const std::vector<glm::vec3>>(col)};
        }
        auto g = std::make_shared<pgg::Geo>();
        g->kind = pgg::GeoKind::Points;
        g->positions = std::make_shared<const std::vector<glm::vec3>>(pos);
        g->pointAttrs = std::move(attrs);
        return g;
    }
};

pgg::GeoPtr emptyGeo(pgg::GeoKind kind) {
    auto g = std::make_shared<pgg::Geo>();
    g->kind = kind;
    g->positions = std::make_shared<const std::vector<glm::vec3>>();
    g->pointAttrs = std::make_shared<pgg::AttrSet>();
    return g;
}

// --- units -----------------------------------------------------------------

struct Unit {
    std::string id, slot, asset;
    std::vector<std::pair<std::string, pgg::Value>> bindings;
    double yawDeg = 0, tx = 0, ty = 0, tz = 0;  // local -> world (R then +t)
};

int needStyle(const std::string& name, const std::string& unit, std::string& err) {
    bool ok = false;
    const int code = style_code(name, ok);
    if (!ok) err = "delve/run [" + unit + "]: unknown style '" + name + "'";
    return code;
}

const IrRoom* roomById(const IrV2& ir, const std::string& id) {
    for (const auto& r : ir.rooms)
        if (r.id == id) return &r;
    return nullptr;
}

void zoneAttrs(PointsBuilder& b, const ZonePiece& p) {
    b.integer("zone", p.zone);
    b.integer("pattern", p.pattern);
    b.integer("seed", p.seed);
    b.f32("t_at_l0", p.t_at_l0);
    b.integer("flip", p.flip);
    b.f32("width", p.width);
    b.f32("module", p.module);
    b.integer("style_a", p.style_a);
    b.integer("style_b", p.style_b);
}

bool expandRoom(const IrRoom& r, double cell, int fill_seed, const std::string& asset, Unit& u,
                std::string& err) {
    u.id = "room:" + r.id;
    u.slot = "room_fill";
    u.asset = asset;
    double minx = 1e300, minz = 1e300;
    for (const auto& [gx, gy] : r.grid) {
        minx = std::min(minx, gx * cell);
        minz = std::min(minz, gy * cell);
    }
    // Convexity from the turn sign vs the winding (rects: all convex).
    double area2 = 0;
    for (size_t i = 0; i < r.grid.size(); ++i) {
        const auto& [x0, y0] = r.grid[i];
        const auto& [x1, y1] = r.grid[(i + 1) % r.grid.size()];
        area2 += (double)x0 * y1 - (double)x1 * y0;
    }
    PointsBuilder b;
    for (size_t i = 0; i < r.grid.size(); ++i) {
        const auto& [gx, gy] = r.grid[i];
        const auto& [hx, hy] = r.grid[(i + 1) % r.grid.size()];
        const auto& [px, py] = r.grid[(i + r.grid.size() - 1) % r.grid.size()];
        b.pt(gx * cell - minx, 0, gy * cell - minz);
        b.f32("edge_len", std::hypot((hx - gx) * cell, (hy - gy) * cell));
        const double cross =
            (double)(gx - px) * (hy - gy) - (double)(gy - py) * (hx - gx);
        b.boolean("convex", cross * area2 > 0);
    }
    pgg::GeoPtr contour = b.build(err);
    if (!contour) return false;
    bool ok = false;
    const int role = role_code(r.role, ok);
    if (!ok) {
        err = "delve/run [" + u.id + "]: unknown role '" + r.role + "'";
        return false;
    }
    const int floor = needStyle(r.floor_style, u.id, err);
    if (!err.empty()) return false;
    const int ceil = needStyle(r.ceil_style, u.id, err);
    if (!err.empty()) return false;
    u.bindings = {{"contour", pgg::Value(contour)},
                  {"h", pgg::Value((float)r.h)},
                  {"role", pgg::Value(role)},
                  {"style_floor", pgg::Value(floor)},
                  {"style_ceil", pgg::Value(ceil)},
                  {"rng_seed", pgg::Value(unit_seed(fill_seed, u.id))}};
    u.tx = minx;
    u.tz = minz;
    return true;
}

bool expandBody(const IrWall& w, const IrV2& ir, double cell, int fill_seed,
                const std::string& asset, const std::map<std::string, const IrDoor*>& doors,
                Unit& u, std::string& err) {
    u.id = w.id;
    u.slot = "wall_body";
    u.asset = asset;
    const double ax = w.g0.first * cell, az = w.g0.second * cell;
    const double bx = w.g1.first * cell, bz = w.g1.second * cell;
    const double span = std::hypot(bx - ax, bz - az);
    const double dx = (bx - ax) / span, dz = (bz - az) / span;
    // The body runs between the pillar faces at the ends (per-end thickness:
    // pillars of a different owner's wall_t step at the joint, 5.2 note).
    const double fx = ax + dx * w.t_end0 / 2, fz = az + dz * w.t_end0 / 2;
    const double len = span - (w.t_end0 + w.t_end1) / 2;
    if (!(len > kEps)) {
        err = "delve/run [" + u.id + "]: wall span " + std::to_string(span) + " <= thick " +
              std::to_string(w.thick);
        return false;
    }
    PointsBuilder seg;
    seg.pt(0, 0, 0);
    seg.pt(len, 0, 0);
    PointsBuilder cuts;
    for (const std::string& did : w.doors) {
        const IrDoor* d = doors.at(did);
        const double la =
            (d->from.first - fx) * dx + (d->from.second - fz) * dz - d->frame;
        const double lb = (d->to.first - fx) * dx + (d->to.second - fz) * dz + d->frame;
        cuts.pt(la, 0, 0);
        cuts.f32("h", d->h);
        cuts.pt(lb, 0, 0);
        cuts.f32("h", d->h);
    }
    pgg::GeoPtr segGeo = seg.build(err);
    if (!segGeo) return false;
    pgg::GeoPtr cutGeo = cuts.build(err);
    if (!cutGeo) return false;
    const IrRoom* owner = roomById(ir, w.owner);
    if (!owner) {
        err = "delve/run [" + u.id + "]: owner room " + w.owner + " missing";
        return false;
    }
    const int style = needStyle(owner->style, u.id, err);
    if (!err.empty()) return false;
    u.bindings = {{"seg", pgg::Value(segGeo)},
                  {"thick", pgg::Value((float)w.thick)},
                  {"h_a", pgg::Value((float)w.h_left)},
                  {"h_b", pgg::Value((float)w.h_right)},
                  {"style", pgg::Value(style)},
                  {"cuts", pgg::Value(cutGeo)},
                  {"rng_seed", pgg::Value(unit_seed(fill_seed, u.id))}};
    u.yawDeg = yawOf(dx, dz);
    u.tx = fx;
    u.tz = fz;
    return true;
}

bool expandFacing(const IrFacing& f, double row_module, int fill_seed,
                  const std::string& asset, Unit& u, std::string& err) {
    u.id = f.id;
    u.slot = "facing";
    u.asset = asset;
    const double dx = f.to.first - f.from.first, dz = f.to.second - f.from.second;
    const double len = std::hypot(dx, dz);
    if (!(len > kEps)) {
        err = "delve/run [" + u.id + "]: zero-length facing seg";
        return false;
    }
    const double ux = dx / len, uz = dz / len;
    // Local n = ±Z: +Z iff the world normal is left... rotation-consistent:
    // yaw maps +X to (ux, uz) and +Z to perp; n must equal ±perp.
    const double px = -uz, pz = ux;
    const double s = f.n.first * px + f.n.second * pz;
    if (std::abs(std::abs(s) - 1.0) > 1e-6) {
        err = "delve/run [" + u.id + "]: facing normal not perpendicular to seg";
        return false;
    }
    PointsBuilder seg;
    seg.pt(0, 0, 0);
    seg.pt(len, 0, 0);
    PointsBuilder cuts;
    for (const auto& c : f.cuts) {
        const double la = (c.a.first - f.from.first) * ux + (c.a.second - f.from.second) * uz;
        const double lb = (c.b.first - f.from.first) * ux + (c.b.second - f.from.second) * uz;
        cuts.pt(std::min(la, lb), 0, 0);
        cuts.f32("h", c.h);
        cuts.pt(std::max(la, lb), 0, 0);
        cuts.f32("h", c.h);
    }
    PointsBuilder zones;
    for (const auto& p : f.zones) {
        zones.pt(p.l0, 0, 0);
        zoneAttrs(zones, p);
        zones.pt(p.l1, 0, 0);
        zoneAttrs(zones, p);
    }
    pgg::GeoPtr segGeo = seg.build(err);
    if (!segGeo) return false;
    pgg::GeoPtr cutGeo = cuts.build(err);
    if (!cutGeo) return false;
    pgg::GeoPtr zoneGeo = zones.build(err);
    if (!zoneGeo) return false;
    const int style = needStyle(f.style, u.id, err);
    if (!err.empty()) return false;
    u.bindings = {{"seg", pgg::Value(segGeo)},
                  {"n", pgg::Value(glm::vec3(0, 0, s > 0 ? 1 : -1))},
                  {"h", pgg::Value((float)f.h)},
                  {"style", pgg::Value(style)},
                  {"module", pgg::Value((float)row_module)},
                  {"cuts", pgg::Value(cutGeo)},
                  {"zones", pgg::Value(zoneGeo)},
                  {"rng_seed", pgg::Value(unit_seed(fill_seed, u.id))}};
    u.yawDeg = yawOf(ux, uz);
    u.tx = f.from.first;
    u.tz = f.from.second;
    return true;
}

bool expandNode(const IrNode& n, const IrV2& ir, double cell, double row_module, int fill_seed,
                const std::string& asset, Unit& u, std::string& err) {
    u.id = n.id;
    u.slot = "node";
    u.asset = asset;
    PointsBuilder faces;
    PointsBuilder zones;
    for (size_t i = 0; i < n.faces.size(); ++i) {
        const IrNodeFace& fc = n.faces[i];
        faces.pt(fc.center.first, 0, fc.center.second);  // already node-local
        faces.vec3("n", fc.n.first, 0, fc.n.second);
        faces.f32("h", fc.h);
        const int style = needStyle(fc.style, u.id, err);
        if (!err.empty()) return false;
        faces.integer("style", style);
        faces.integer("face", (int)i);
        for (const auto& p : fc.zones) {
            zones.pt(p.l0, 0, 0);
            zoneAttrs(zones, p);
            zones.integer("face", (int)i);
            zones.pt(p.l1, 0, 0);
            zoneAttrs(zones, p);
            zones.integer("face", (int)i);
        }
    }
    pgg::GeoPtr faceGeo = faces.build(err);
    if (!faceGeo) return false;
    pgg::GeoPtr zoneGeo = zones.build(err);
    if (!zoneGeo) return false;
    const IrRoom* owner = roomById(ir, n.owner);
    if (!owner) {
        err = "delve/run [" + u.id + "]: owner room " + n.owner + " missing";
        return false;
    }
    const int style = needStyle(owner->style, u.id, err);
    if (!err.empty()) return false;
    u.bindings = {{"faces", pgg::Value(faceGeo)},
                  {"thick", pgg::Value((float)n.thick)},
                  {"h_pillar", pgg::Value((float)n.h_pillar)},
                  {"style", pgg::Value(style)},
                  {"module", pgg::Value((float)row_module)},
                  {"zones", pgg::Value(zoneGeo)},
                  {"rng_seed", pgg::Value(unit_seed(fill_seed, u.id))}};
    u.tx = n.at.first * cell;
    u.tz = n.at.second * cell;
    return true;
}

bool expandDoor(const IrDoor& d, int fill_seed, const std::string& asset, Unit& u,
                std::string& err) {
    u.id = d.id;
    u.slot = "door";
    u.asset = asset;
    const double dx = d.to.first - d.from.first, dz = d.to.second - d.from.second;
    const double len = std::hypot(dx, dz);
    if (!(len > kEps)) {
        err = "delve/run [" + u.id + "]: zero-length door seg";
        return false;
    }
    PointsBuilder seg;
    seg.pt(0, 0, 0);
    seg.pt(len, 0, 0);
    pgg::GeoPtr segGeo = seg.build(err);
    if (!segGeo) return false;
    u.bindings = {{"seg", pgg::Value(segGeo)},
                  {"h", pgg::Value((float)d.h)},
                  {"frame", pgg::Value((float)d.frame)},
                  {"thick", pgg::Value((float)d.thick)},
                  {"dtype", pgg::Value(d.dtype)},
                  {"rng_seed", pgg::Value(unit_seed(fill_seed, u.id))}};
    u.yawDeg = yawOf(dx / len, dz / len);
    u.tx = d.from.first;
    u.tz = d.from.second;
    return true;
}

// v1 lamp placement, "ceil" mode: centered inset grid over the room bbox,
// nx = max(1, round(sx / step)) per axis, mount on the ceiling plane.
// Positions outside a figured room's contour are skipped (rects: never
// triggers). Ids are room-local (D3): deco:lamp:<room>:<k>.
bool pointInRoomGrid(const IrRoom& r, double cell, double x, double z) {
    bool inside = false;
    const size_t n = r.grid.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const double xi = r.grid[i].first * cell, yi = r.grid[i].second * cell;
        const double xj = r.grid[j].first * cell, yj = r.grid[j].second * cell;
        if ((yi > z) != (yj > z) && x < (xj - xi) * (z - yi) / (yj - yi) + xi) inside = !inside;
    }
    return inside;
}

// "wall" mode (sconces): walk the room's facings (wall faces already offset
// into the room, normal points inside). Positions run along each facing with
// a 0.5 m end inset and `step` spacing, y = min(1.9, h - 0.5), @n = inward
// horizontal normal, mount point 0.08 m off the wall face. Positions within
// 0.4 m of a door cut are skipped. Style = the room's wall style.
bool expandLampsWall(const IrRoom& r, const IrV2& ir, double step, int fill_seed,
                     const std::string& asset, int& k, std::vector<Unit>& units,
                     std::string& err) {
    bool ok = false;
    const int style = style_code(r.style, ok);
    if (!ok) {
        err = "delve/run [room:" + r.id + "]: unknown style '" + r.style + "'";
        return false;
    }
    const double y = std::min(1.9, r.h - 0.5);
    for (const auto& f : ir.facings) {
        if (f.room != r.id) continue;
        const double dx = f.to.first - f.from.first, dz = f.to.second - f.from.second;
        const double len = std::hypot(dx, dz);
        if (!(len > 1.0)) continue;  // corner sliver: no lamp
        const double ux = dx / len, uz = dz / len;
        const double inset = 0.5;
        const double usable = len - 2 * inset;
        const int cnt = std::max(1, (int)std::floor(usable / step) + 1);
        for (int i = 0; i < cnt; ++i) {
            const double s = inset + usable * (i + 0.5) / cnt;
            bool blocked = false;
            for (const auto& c : f.cuts) {
                const double ca = (c.a.first - f.from.first) * ux + (c.a.second - f.from.second) * uz;
                const double cb = (c.b.first - f.from.first) * ux + (c.b.second - f.from.second) * uz;
                if (s > std::min(ca, cb) - 0.4 && s < std::max(ca, cb) + 0.4) {
                    blocked = true;
                    break;
                }
            }
            if (blocked) continue;
            Unit u;
            u.id = "deco:lamp:" + r.id + ":" + std::to_string(k++);
            u.slot = "decor:lamp";
            u.asset = asset;
            PointsBuilder p;
            p.pt(0, 0, 0);
            p.vec3("n", f.n.first, 0, f.n.second);
            pgg::GeoPtr pGeo = p.build(err);
            if (!pGeo) return false;
            u.bindings = {{"p", pgg::Value(pGeo)},
                          {"style", pgg::Value(style)},
                          {"tag", pgg::Value(1)},
                          {"rng_seed", pgg::Value(unit_seed(fill_seed, u.id))}};
            u.tx = f.from.first + ux * s + f.n.first * 0.08;
            u.ty = y;
            u.tz = f.from.second + uz * s + f.n.second * 0.08;
            units.push_back(std::move(u));
        }
    }
    return true;
}

bool expandLamps(const IrRoom& r, const IrV2& ir, double cell, double step,
                 const std::string& place, int fill_seed, const std::string& asset,
                 std::vector<Unit>& units, std::string& err) {
    int k = 0;
    if (place == "wall") return expandLampsWall(r, ir, step, fill_seed, asset, k, units, err);
    double x0 = 1e300, x1 = -1e300, z0 = 1e300, z1 = -1e300;
    for (const auto& [gx, gy] : r.grid) {
        x0 = std::min(x0, gx * cell);
        x1 = std::max(x1, gx * cell);
        z0 = std::min(z0, gy * cell);
        z1 = std::max(z1, gy * cell);
    }
    const int nx = std::max(1, (int)std::llround((x1 - x0) / step));
    const int nz = std::max(1, (int)std::llround((z1 - z0) / step));
    bool ok = false;
    const int style = style_code(r.ceil_style, ok);
    if (!ok) {
        err = "delve/run [room:" + r.id + "]: unknown style '" + r.ceil_style +
              "'";
        return false;
    }
    for (int i = 0; i < nx; ++i)
        for (int j = 0; j < nz; ++j) {
            const double lx = x0 + (x1 - x0) * (i + 0.5) / nx;
            const double lz = z0 + (z1 - z0) * (j + 0.5) / nz;
            if (!pointInRoomGrid(r, cell, lx, lz)) continue;  // notch of a figured room
            Unit u;
            u.id = "deco:lamp:" + r.id + ":" + std::to_string(k++);
            u.slot = "decor:lamp";
            u.asset = asset;
            PointsBuilder p;
            p.pt(0, 0, 0);
            p.vec3("n", 0, -1, 0);
            pgg::GeoPtr pGeo = p.build(err);
            if (!pGeo) return false;
            u.bindings = {{"p", pgg::Value(pGeo)},
                          {"style", pgg::Value(style)},
                          {"tag", pgg::Value(1)},
                          {"rng_seed", pgg::Value(unit_seed(fill_seed, u.id))}};
            u.tx = lx;
            u.ty = r.h;
            u.tz = lz;
            units.push_back(std::move(u));
        }
    return true;
}

// --- run -------------------------------------------------------------------

bool runUnit(const Unit& u, const std::vector<std::string>& roots, unsigned threads,
             pgg::GeoPtr& mesh, pgg::GeoPtr& anchors, std::string& err) {
    pgg::RunParams rp;
    rp.values = u.bindings;
    rp.importRoots = roots;
    rp.threads = threads;
    const pgg::RunResult r = pgg::runFile(u.asset, rp);
    if (r.hasErrors()) {
        err = "delve/run [" + u.id + " (" + u.slot + ", " + u.asset + ")]:";
        for (const auto& d : r.diagnostics)
            if (!d.isWarning) err += "\n  [" + d.code + "] " + d.message;
        return false;
    }
    const pgg::Value* meshV = nullptr;
    const pgg::Value* anchorsV = nullptr;
    for (const auto& o : r.outputs) {
        if (o.name == "mesh") meshV = &o.value;
        if (o.name == "anchors") anchorsV = &o.value;
    }
    // First-run output-kind check (slots §4: PGG exposes no static types).
    auto kindOf = [](const pgg::Value* v) {
        if (!v || pgg::valueBase(*v) != pgg::ScalarType::Geo) return "non-geo";
        switch (pgg::asGeo(*v)->kind) {
            case pgg::GeoKind::Mesh: return "geo<mesh>";
            case pgg::GeoKind::Points: return "geo<points>";
            default: return "geo<other>";
        }
    };
    if (!meshV || kindOf(meshV) != std::string("geo<mesh>")) {
        err = "delve/slot [" + u.id + " (" + u.slot + ")]: output 'mesh' is " +
              kindOf(meshV) + ", required geo<mesh>";
        return false;
    }
    if (!anchorsV || kindOf(anchorsV) != std::string("geo<points>")) {
        err = "delve/slot [" + u.id + " (" + u.slot + ")]: output 'anchors' is " +
              kindOf(anchorsV) + ", required geo<points>";
        return false;
    }
    mesh = pgg::asGeo(*meshV);
    anchors = pgg::asGeo(*anchorsV);
    return true;
}

// --- assembly: rigid frames ------------------------------------------------
// Rotation about +Y (PGG convention, probed): x' = x c + z s,
// z' = -x s + z c. Points translate, Vector/Normal rotate, the rest copies.

glm::vec3 rotY(const glm::vec3& p, double c, double s) {
    return {(float)(p.x * c + p.z * s), p.y, (float)(-p.x * s + p.z * c)};
}

pgg::GeoPtr rigidGeo(const pgg::GeoPtr& g, double yawDeg, double tx, double ty, double tz,
                     const std::string& unit, std::string& err) {
    if (g->instanceSources) {
        err = "delve/run [" + unit + "]: instances in unit output (unsupported in v1 assembly)";
        return {};
    }
    const double a = yawDeg * std::numbers::pi / 180.0;
    const double c = std::cos(a), s = std::sin(a);
    auto out = std::make_shared<pgg::Geo>(*g);  // shallow: topology/groups shared
    auto pos = std::make_shared<std::vector<glm::vec3>>();
    pos->reserve(g->pointCount());
    for (const auto& p : *g->positions) {
        const glm::vec3 q = rotY(p, c, s);
        pos->emplace_back(q.x + (float)tx, q.y + (float)ty, q.z + (float)tz);
    }
    out->positions = std::move(pos);
    if (g->normals) {
        auto n = std::make_shared<std::vector<glm::vec3>>();
        n->reserve(g->normals->size());
        for (const auto& v : *g->normals) n->push_back(rotY(v, c, s));
        out->normals = std::move(n);
    }
    auto rotAttrs = [&](const std::shared_ptr<const pgg::AttrSet>& attrs) {
        if (!attrs) return attrs;
        auto set = std::make_shared<pgg::AttrSet>(*attrs);  // shallow column copy
        for (auto& [name, col] : set->columns) {
            if (col.typeInfo != pgg::AttrTypeInfo::Vector &&
                col.typeInfo != pgg::AttrTypeInfo::Normal &&
                col.typeInfo != pgg::AttrTypeInfo::Point)
                continue;
            auto* v3 = std::get_if<std::shared_ptr<const std::vector<glm::vec3>>>(&col.data);
            if (!v3) continue;  // mistagged column: leave alone (clip/views never tag)
            auto v = std::make_shared<std::vector<glm::vec3>>();
            v->reserve((*v3)->size());
            const bool move = col.typeInfo == pgg::AttrTypeInfo::Point;
            for (const auto& p : **v3) {
                const glm::vec3 q = rotY(p, c, s);
                v->emplace_back(q.x + (move ? (float)tx : 0), q.y + (move ? (float)ty : 0),
                                q.z + (move ? (float)tz : 0));
            }
            col.data = std::move(v);
        }
        return std::shared_ptr<const pgg::AttrSet>(std::move(set));
    };
    out->pointAttrs = rotAttrs(g->pointAttrs);
    out->cornerAttrs = rotAttrs(g->cornerAttrs);
    out->faceAttrs = rotAttrs(g->faceAttrs);
    return out;
}

// --- assembly: merge -------------------------------------------------------

size_t domainCount(const pgg::GeoPtr& g, int domain) {  // 0 pt, 1 corner, 2 face
    return domain == 0 ? g->pointCount() : (domain == 1 ? g->cornerCount() : g->faceCount());
}

const pgg::AttrSet* domainAttrs(const pgg::GeoPtr& g, int domain) {
    const auto& ptr =
        domain == 0 ? g->pointAttrs : (domain == 1 ? g->cornerAttrs : g->faceAttrs);
    return ptr.get();
}

bool mergeDomain(const std::vector<pgg::GeoPtr>& parts, int domain,
                 std::shared_ptr<pgg::AttrSet>& out, std::string& err) {
    std::map<std::string, const pgg::AttrColumn*> first;  // sorted = deterministic
    for (const auto& g : parts) {
        const pgg::AttrSet* set = domainAttrs(g, domain);
        if (!set) continue;
        for (const auto& [name, col] : set->columns)
            if (!first.count(name)) first[name] = &col;
    }
    out = std::make_shared<pgg::AttrSet>();
    for (const auto& [name, col0] : first) {
        const size_t tag = col0->data.index();
        // Concatenate per-type (neutral fill where a part lacks the column).
        if (tag == 0) {
            auto v = std::make_shared<std::vector<float>>();
            for (const auto& g : parts) {
                const pgg::AttrSet* set = domainAttrs(g, domain);
                const pgg::AttrColumn* col = set ? set->find(name) : nullptr;
                if (col && col->data.index() != tag) {
                    err = "delve/run: column '" + name + "' type mismatch in assembly merge";
                    return false;
                }
                if (col)
                    v->insert(v->end(), std::get<0>(col->data)->begin(),
                              std::get<0>(col->data)->end());
                else
                    v->insert(v->end(), domainCount(g, domain), 0.0f);
            }
            out->columns[name] = pgg::AttrColumn{std::move(v), col0->typeInfo};
        } else if (tag == 1) {
            auto v = std::make_shared<std::vector<int64_t>>();
            for (const auto& g : parts) {
                const pgg::AttrSet* set = domainAttrs(g, domain);
                const pgg::AttrColumn* col = set ? set->find(name) : nullptr;
                if (col && col->data.index() != tag) {
                    err = "delve/run: column '" + name + "' type mismatch in assembly merge";
                    return false;
                }
                if (col)
                    v->insert(v->end(), std::get<1>(col->data)->begin(),
                              std::get<1>(col->data)->end());
                else
                    v->insert(v->end(), domainCount(g, domain), 0);
            }
            out->columns[name] = pgg::AttrColumn{std::move(v), col0->typeInfo};
        } else if (tag == 2) {
            auto v = std::make_shared<std::vector<uint8_t>>();
            for (const auto& g : parts) {
                const pgg::AttrSet* set = domainAttrs(g, domain);
                const pgg::AttrColumn* col = set ? set->find(name) : nullptr;
                if (col && col->data.index() != tag) {
                    err = "delve/run: column '" + name + "' type mismatch in assembly merge";
                    return false;
                }
                if (col)
                    v->insert(v->end(), std::get<2>(col->data)->begin(),
                              std::get<2>(col->data)->end());
                else
                    v->insert(v->end(), domainCount(g, domain), 0);
            }
            out->columns[name] = pgg::AttrColumn{std::move(v), col0->typeInfo};
        } else if (tag == 4) {
            auto v = std::make_shared<std::vector<glm::vec3>>();
            for (const auto& g : parts) {
                const pgg::AttrSet* set = domainAttrs(g, domain);
                const pgg::AttrColumn* col = set ? set->find(name) : nullptr;
                if (col && col->data.index() != tag) {
                    err = "delve/run: column '" + name + "' type mismatch in assembly merge";
                    return false;
                }
                if (col)
                    v->insert(v->end(), std::get<4>(col->data)->begin(),
                              std::get<4>(col->data)->end());
                else
                    v->insert(v->end(), domainCount(g, domain), glm::vec3(0));
            }
            out->columns[name] = pgg::AttrColumn{std::move(v), col0->typeInfo};
        } else if (tag == 6) {
            auto v = std::make_shared<std::vector<std::string>>();
            for (const auto& g : parts) {
                const pgg::AttrSet* set = domainAttrs(g, domain);
                const pgg::AttrColumn* col = set ? set->find(name) : nullptr;
                if (col && col->data.index() != tag) {
                    err = "delve/run: column '" + name + "' type mismatch in assembly merge";
                    return false;
                }
                if (col)
                    v->insert(v->end(), std::get<6>(col->data)->begin(),
                              std::get<6>(col->data)->end());
                else
                    v->insert(v->end(), domainCount(g, domain), std::string());
            }
            out->columns[name] = pgg::AttrColumn{std::move(v), col0->typeInfo};
        } else {
            err = "delve/run: column '" + name + "' (vec2/vec4) unsupported in assembly merge";
            return false;
        }
    }
    return true;
}

bool mergeGeos(const std::vector<pgg::GeoPtr>& parts, pgg::GeoKind kind, pgg::GeoPtr& out,
               std::string& err) {
    if (parts.empty()) {
        out = emptyGeo(kind);
        return true;
    }
    if (parts.size() == 1) {
        out = parts[0];
        return true;
    }
    for (const auto& g : parts) {
        if (g->kind != kind) {
            err = "delve/run: mixed geo kinds in assembly merge";
            return false;
        }
        if (g->instanceSources || g->pointGroups || g->cornerGroups || g->faceGroups ||
            g->detailAttrs || g->detailGroups) {
            err = "delve/run: groups/detail/instances unsupported in assembly merge";
            return false;
        }
    }
    auto g = std::make_shared<pgg::Geo>();
    g->kind = kind;
    auto pos = std::make_shared<std::vector<glm::vec3>>();
    for (const auto& p : parts) pos->insert(pos->end(), p->positions->begin(), p->positions->end());
    g->positions = std::move(pos);
    bool anyN = false, allN = true;
    for (const auto& p : parts) {
        if (p->pointCount() == 0) continue;  // contributes no values
        if (p->normals)
            anyN = true;
        else
            allN = false;
    }
    if (anyN && !allN) {
        err = "delve/run: mixed normals presence in assembly merge";
        return false;
    }
    if (anyN) {
        auto n = std::make_shared<std::vector<glm::vec3>>();
        for (const auto& p : parts) {
            if (p->pointCount() == 0) continue;  // contributes no values
            n->insert(n->end(), p->normals->begin(), p->normals->end());
        }
        g->normals = std::move(n);
    }
    auto corners = std::make_shared<std::vector<int32_t>>();
    auto offsets = std::make_shared<std::vector<int32_t>>();
    int32_t poff = 0, coff = 0;
    for (const auto& p : parts) {
        if (p->cornerVerts)
            for (int32_t v : *p->cornerVerts) corners->push_back(v + poff);
        if (p->faceOffsets) {
            if (p->faceOffsets->empty() || p->faceOffsets->front() != 0) {
                err = "delve/run: corrupt face offsets in assembly merge";
                return false;
            }
            for (size_t i = (offsets->empty() ? 0 : 1); i < p->faceOffsets->size(); ++i)
                offsets->push_back((*p->faceOffsets)[i] + coff);
        }
        poff += (int32_t)p->pointCount();
        coff += (int32_t)p->cornerCount();
    }
    g->cornerVerts = std::move(corners);
    g->faceOffsets = std::move(offsets);
    for (int domain = 0; domain < 3; ++domain) {
        std::shared_ptr<pgg::AttrSet> set;
        if (!mergeDomain(parts, domain, set, err)) return false;
        if (domain == 0)
            g->pointAttrs = std::move(set);
        else if (domain == 1)
            g->cornerAttrs = std::move(set);
        else
            g->faceAttrs = std::move(set);
    }
    out = std::move(g);
    return true;
}

// --- labels ----------------------------------------------------------------

pgg::GeoPtr labelAnchors(const pgg::GeoPtr& g, const std::string& unit, std::string& err) {
    if (g->pointCount() == 0) return g;
    const pgg::AttrColumn* kind = g->pointAttrs ? g->pointAttrs->find("kind") : nullptr;
    const auto* kinds = kind ? std::get_if<std::shared_ptr<const std::vector<int64_t>>>(&kind->data)
                             : nullptr;
    if (!kinds) {
        err = "delve/run [" + unit + "]: anchors without an int @kind column";
        return {};
    }
    auto labels = std::make_shared<std::vector<std::string>>();
    for (int64_t k : **kinds) {
        const char* name = k == 1 ? "light" : (k == 2 ? "spawn" : (k == 3 ? "poi" : nullptr));
        if (!name) {
            err = "delve/run [" + unit + "]: anchor with unknown @kind " + std::to_string(k);
            return {};
        }
        labels->push_back(unit + "#" + name);
    }
    auto out = std::make_shared<pgg::Geo>(*g);
    auto set = std::make_shared<pgg::AttrSet>(*g->pointAttrs);
    set->columns["label"] = pgg::AttrColumn{std::move(labels)};
    out->pointAttrs = std::move(set);
    return out;
}

}  // namespace

bool fill_level(const IrV2& ir, const Project& project, const FillOpts& opts, FillResult& out,
                std::string& err) {
    out = FillResult{};
    if (opts.delve_assets.empty()) {
        err = "delve/run: FillOpts::delve_assets is empty";
        return false;
    }
    std::vector<std::string> roots;
    for (const auto& r : project.asset_roots)
        roots.push_back((std::filesystem::path(project.dir) / r).string());
    roots.push_back(opts.delve_assets);

    // Needed slots (only kinds with units; lamps iff some room exists).
    std::map<std::string, size_t> need = {{"room_fill", ir.rooms.size()},
                                          {"wall_body", ir.walls.size()},
                                          {"facing", ir.facings.size()},
                                          {"node", ir.nodes.size()},
                                          {"door", ir.doors.size()},
                                          {"decor:lamp", ir.rooms.size()}};
    std::map<std::string, std::string> assets;  // slot -> resolved file
    for (const auto& [slot, n] : need) {
        if (n == 0) continue;
        const auto sit = project.slots.find(slot);
        if (sit == project.slots.end()) {
            err = "delve/slot: project has no '" + slot + "' entry (" + std::to_string(n) +
                  " units need it)";
            return false;
        }
        std::string found;
        for (const auto& root : roots) {
            const std::string cand = (std::filesystem::path(root) / sit->second).string();
            if (std::filesystem::exists(cand)) {
                found = cand;
                break;
            }
        }
        if (found.empty()) {
            err = "delve/slot: asset '" + sit->second + "' for slot '" + slot + "' not found";
            return false;
        }
        std::vector<SlotDiag> ds;
        if (!check_asset(slot, found, roots, ds)) {
            err = "delve/slot [" + slot + " " + found + "]:";
            for (const auto& d : ds)
                if (!d.warning) err += "\n  [" + d.code + "] " + d.message;
            return false;
        }
        assets[slot] = found;
    }

    // F8: one content key per slot (R-A7 covers the whole import closure).
    std::map<std::string, uint64_t> assetKeys;
    if (opts.cache) {
        for (const auto& [slot, file] : assets) {
            uint64_t ak = 0;
            if (!asset_content_key(file, roots, ak, err)) return false;
            assetKeys[slot] = ak;
        }
    }

    std::map<std::string, const IrDoor*> doors;
    for (const auto& d : ir.doors) doors[d.id] = &d;
    // 5.6: v1 projects fill from the split fill seed; the v0 path keeps using
    // the project seed directly (docs/project_v1.md).
    const int fseed =
        project.format == kProjectFormatV1 ? fill_seed_v1(project.seed) : project.seed;
    std::vector<Unit> units;
    for (const auto& r : ir.rooms) {
        Unit u;
        if (!expandRoom(r, project.fill.cell, fseed, assets["room_fill"], u, err))
            return false;
        units.push_back(std::move(u));
    }
    for (const auto& w : ir.walls) {
        Unit u;
        if (!expandBody(w, ir, project.fill.cell, fseed, assets["wall_body"], doors, u,
                        err))
            return false;
        units.push_back(std::move(u));
    }
    for (const auto& f : ir.facings) {
        Unit u;
        if (!expandFacing(f, project.fill.row_module, fseed, assets["facing"], u, err))
            return false;
        units.push_back(std::move(u));
    }
    for (const auto& n : ir.nodes) {
        Unit u;
        if (!expandNode(n, ir, project.fill.cell, project.fill.row_module, fseed,
                        assets["node"], u, err))
            return false;
        units.push_back(std::move(u));
    }
    for (const auto& d : ir.doors) {
        Unit u;
        if (!expandDoor(d, fseed, assets["door"], u, err)) return false;
        units.push_back(std::move(u));
    }
    for (const auto& r : ir.rooms) {
        if (!expandLamps(r, ir, project.fill.cell, project.fill.lamp_step,
                         project.fill.lamp_place, fseed, assets["decor:lamp"], units, err))
            return false;
    }

    std::vector<pgg::GeoPtr> meshes, anchors;
    if (!assets.empty())
        pgg::appendImportRoot(roots, pgg::findProductLibRoot(assets.begin()->second));
    size_t meshOff = 0, anchorsOff = 0;
    for (const auto& u : units) {
        pgg::GeoPtr mesh, anch;
        if (opts.cache) {
            UnitKey key;
            if (!unit_key(u.slot, assetKeys[u.slot], u.bindings, key, err)) return false;
            UnitCache::Entry e;
            if (opts.cache->lookup(key, e)) {
                mesh = e.mesh;
                anch = e.anchors;
                out.stats.reused.push_back(u.id);
            } else {
                if (!runUnit(u, roots, opts.threads, mesh, anch, err)) return false;
                opts.cache->store(key, mesh, anch);
                out.stats.reran.push_back(u.id);
            }
        } else {
            if (!runUnit(u, roots, opts.threads, mesh, anch, err)) return false;
        }
        mesh = rigidGeo(mesh, u.yawDeg, u.tx, u.ty, u.tz, u.id, err);
        if (!mesh) return false;
        anch = rigidGeo(anch, u.yawDeg, u.tx, u.ty, u.tz, u.id, err);
        if (!anch) return false;
        anch = labelAnchors(anch, u.id, err);
        if (!anch) return false;
        FillResult::UnitSpan span;
        span.id = u.id;
        span.slot = u.slot;
        span.meshBegin = meshOff;
        span.meshEnd = meshOff + mesh->pointCount();
        span.anchorsBegin = anchorsOff;
        span.anchorsEnd = anchorsOff + anch->pointCount();
        meshOff = span.meshEnd;
        anchorsOff = span.anchorsEnd;
        out.units.push_back(std::move(span));
        meshes.push_back(std::move(mesh));
        anchors.push_back(std::move(anch));
    }
    if (!mergeGeos(meshes, pgg::GeoKind::Mesh, out.mesh, err)) return false;
    if (!mergeGeos(anchors, pgg::GeoKind::Points, out.anchors, err)) return false;
    out.stats.rooms = ir.rooms.size();
    out.stats.bodies = ir.walls.size();
    out.stats.facings = ir.facings.size();
    out.stats.nodes = ir.nodes.size();
    out.stats.doors = ir.doors.size();
    size_t lamps = 0;
    for (const auto& u : units)
        if (u.slot == "decor:lamp") ++lamps;
    out.stats.lamps = lamps;
    return true;
}

}  // namespace delve
