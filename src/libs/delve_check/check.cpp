// Delve F11 geometric checks (numbers, no pictures).

#include "check.h"

#include <array>
#include <cmath>
#include <map>
#include <set>

#include "pgg/src/eval/geometry.h"

namespace delve {
namespace {

constexpr size_t kCap = 10;  // max diags per check (fail-fast on spam)
constexpr double kTol = 1e-3;

void push(std::vector<CheckDiag>& ds, size_t mark, const std::string& check,
          const std::string& msg) {
    if (ds.size() < mark + kCap) ds.push_back({check, "F11/" + check + ": " + msg});
}

std::string pt3(const glm::vec3& p) {
    return "(" + std::to_string(p.x) + ", " + std::to_string(p.y) + ", " + std::to_string(p.z) +
           ")";
}

const std::vector<int64_t>* styleCol(const pgg::GeoPtr& g) {
    if (!g->pointAttrs) return nullptr;
    const pgg::AttrColumn* c = g->pointAttrs->find("style");
    if (!c) return nullptr;
    auto* v = std::get_if<std::shared_ptr<const std::vector<int64_t>>>(&c->data);
    return v ? v->get() : nullptr;
}

// --- elements (connected face groups) --------------------------------------

struct DSU {
    std::vector<int> p;
    int find(int x) { return p[x] == x ? x : p[x] = find(p[x]); }
    void unite(int a, int b) { p[find(a)] = find(b); }
};

struct Element {
    std::vector<int> pts;  // corner points (dupes ok for min/max)
    glm::vec3 bmin, bmax;
    std::set<int64_t> styles;
};

struct FaceInfo {
    std::vector<int> pts;
    glm::vec3 n{0};  // Newell normal (unnormalized; zero if degenerate)
    glm::vec3 c{0};  // centroid
};

glm::vec3 newell(const std::vector<int>& pts, const std::vector<glm::vec3>& pos) {
    glm::vec3 n(0);
    for (size_t i = 0; i < pts.size(); ++i) {
        const glm::vec3& a = pos[pts[i]];
        const glm::vec3& b = pos[pts[(i + 1) % pts.size()]];
        n.x += (a.y - b.y) * (a.z + b.z);
        n.y += (a.z - b.z) * (a.x + b.x);
        n.z += (a.x - b.x) * (a.y + b.y);
    }
    return n;
}

std::vector<FaceInfo> spanFaces(const pgg::GeoPtr& mesh, size_t begin, size_t end) {
    std::vector<FaceInfo> out;
    if (!mesh->cornerVerts || !mesh->faceOffsets || mesh->faceOffsets->size() < 2) return out;
    const auto& corners = *mesh->cornerVerts;
    const auto& offs = *mesh->faceOffsets;
    for (size_t f = 0; f + 1 < offs.size(); ++f) {
        if (offs[f + 1] == offs[f]) continue;
        FaceInfo fi;
        bool inside = true;
        for (int32_t c = offs[f]; c < offs[f + 1]; ++c) {
            if (corners[c] < (int32_t)begin || corners[c] >= (int32_t)end) inside = false;
            fi.pts.push_back(corners[c]);
        }
        if (!inside) continue;
        for (int pi : fi.pts) fi.c += (*mesh->positions)[pi];
        fi.c /= (float)fi.pts.size();
        fi.n = newell(fi.pts, *mesh->positions);
        out.push_back(std::move(fi));
    }
    return out;
}

std::vector<Element> groupFaces(const pgg::GeoPtr& mesh, size_t begin, size_t end,
                                const std::vector<int64_t>* styles) {
    std::vector<Element> out;
    const std::vector<FaceInfo> faces = spanFaces(mesh, begin, end);
    DSU dsu;
    dsu.p.resize(faces.size());
    for (size_t i = 0; i < faces.size(); ++i) dsu.p[i] = (int)i;
    // Union faces sharing an edge (2+ point INDICES). Same-box faces share
    // real vertices (box() is indexed: 8 corners); merged boxes never share
    // indices (merge does not weld), so touching boxes stay separate with no
    // coplanarity test. Triangulation halves and clip caps share indices too.
    std::map<int, std::vector<int>> at;
    for (size_t i = 0; i < faces.size(); ++i)
        for (int pi : faces[i].pts) at[pi].push_back((int)i);
    std::map<std::pair<int, int>, int> shared;
    for (const auto& [key, vec] : at)
        for (size_t i = 0; i < vec.size(); ++i)
            for (size_t j = i + 1; j < vec.size(); ++j)
                ++shared[{std::min(vec[i], vec[j]), std::max(vec[i], vec[j])}];
    for (const auto& [pair, count] : shared)
        if (count >= 2) dsu.unite(pair.first, pair.second);
    std::map<int, size_t> comp;
    for (size_t i = 0; i < faces.size(); ++i) {
        const int root = dsu.find((int)i);
        if (!comp.count(root)) {
            comp[root] = out.size();
            out.push_back(Element{});
            out.back().bmin = glm::vec3(1e30f);
            out.back().bmax = glm::vec3(-1e30f);
        }
        Element& e = out[comp[root]];
        for (int pi : faces[i].pts) {
            e.pts.push_back(pi);
            const glm::vec3& p = (*mesh->positions)[pi];
            e.bmin = glm::min(e.bmin, p);
            e.bmax = glm::max(e.bmax, p);
            if (styles && pi < (int)styles->size()) e.styles.insert((*styles)[pi]);
        }
    }
    return out;
}

// Coplanar same-normal 2D overlap area of two faces: exact convex clip
// (Sutherland-Hodgman) in the face plane. > 0 means duplicate surface
// (z-fighting); touching boxes have opposite normals and never match. A bbox
// test is not enough: rotated rects (e.g. yaw-spun props) touch at a corner
// yet their 2D bboxes overlap by ~1e-4 m2, a false "double".
double doubleArea(const FaceInfo& a, const FaceInfo& b, const std::vector<glm::vec3>& pos) {
    const double la = glm::length(a.n), lb = glm::length(b.n);
    if (la < 1e-12 || lb < 1e-12) return 0;
    if (glm::dot(a.n, b.n) / (la * lb) < 1.0 - 1e-6) return 0;  // not same-normal
    const glm::vec3 un = a.n / (float)la;
    if (std::abs(glm::dot(un, b.c - a.c)) > 1e-6) return 0;  // not coplanar
    // 2D basis on the plane.
    const glm::vec3 ref = std::abs(un.x) < 0.9 ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
    const glm::vec3 e0 = glm::normalize(glm::cross(un, ref));
    const glm::vec3 e1 = glm::cross(un, e0);
    auto project = [&](const FaceInfo& f) {
        std::vector<glm::dvec2> out;
        out.reserve(f.pts.size());
        for (int pi : f.pts) {
            const glm::dvec3 d = glm::dvec3(pos[pi]) - glm::dvec3(a.c);
            out.emplace_back(glm::dot(d, glm::dvec3(e0)), glm::dot(d, glm::dvec3(e1)));
        }
        return out;
    };
    const std::vector<glm::dvec2> pa = project(a);
    const std::vector<glm::dvec2> pb = project(b);
    double bArea2 = 0;  // signed area x2: winding sign for the inside test
    for (size_t i = 0; i < pb.size(); ++i) {
        const glm::dvec2& p = pb[i];
        const glm::dvec2& q = pb[(i + 1) % pb.size()];
        bArea2 += p.x * q.y - q.x * p.y;
    }
    if (std::abs(bArea2) < 1e-12) return 0;
    const double wind = bArea2 > 0 ? 1.0 : -1.0;
    // Clip a's polygon by b's edges (both convex for box/ngon faces).
    std::vector<glm::dvec2> poly = pa;
    for (size_t i = 0; i < pb.size() && poly.size() >= 3; ++i) {
        const glm::dvec2 p = pb[i];
        const glm::dvec2 q = pb[(i + 1) % pb.size()];
        const glm::dvec2 edge = q - p;
        auto side = [&](const glm::dvec2& r) {
            return wind * (edge.x * (r.y - p.y) - edge.y * (r.x - p.x));
        };
        std::vector<glm::dvec2> next;
        for (size_t j = 0; j < poly.size(); ++j) {
            const glm::dvec2 s = poly[j];
            const glm::dvec2 t = poly[(j + 1) % poly.size()];
            const double ss = side(s), st = side(t);
            if (ss >= 0) next.push_back(s);
            if ((ss >= 0) != (st >= 0)) {
                const double k = ss / (ss - st);
                next.emplace_back(s.x + (t.x - s.x) * k, s.y + (t.y - s.y) * k);
            }
        }
        poly = std::move(next);
    }
    double area2 = 0;
    for (size_t i = 0; i < poly.size(); ++i) {
        const glm::dvec2& p = poly[i];
        const glm::dvec2& q = poly[(i + 1) % poly.size()];
        area2 += p.x * q.y - q.x * p.y;
    }
    return std::abs(area2) * 0.5;
}

// Zone boundary (slots §3) at the element's course.
double zoneBoundary(const ZonePiece& p, double yc) {
    if (p.pattern == 0) return p.width * 0.5;
    const double course = std::floor(yc / p.module);
    const double half = p.module * 0.5;
    const bool even = std::fmod(course, 2.0) == 0.0;
    return p.width * 0.5 + (even ? -half : half);
}

}  // namespace

bool check_passage(const IrV2& ir, const Project& project, std::vector<CheckDiag>& diags) {
    const size_t mark = diags.size();
    for (const auto& [id, clear] : ir.corridor_clear)
        if (clear < project.fill.min_passage - 1e-9)
            push(diags, mark, "passage",
                 "corridor " + id + ": clear width " + std::to_string(clear) +
                     " < min_passage " + std::to_string(project.fill.min_passage));
    for (const auto& d : ir.doors)
        if (d.clear < project.fill.min_opening - 1e-9)
            push(diags, mark, "passage",
                 "door " + d.id + ": clear width " + std::to_string(d.clear) + " < min_opening " +
                     std::to_string(project.fill.min_opening));
    return diags.size() == mark;
}

bool check_opening_voids(const IrV2& ir, const FillResult& fill, std::vector<CheckDiag>& diags) {
    const size_t mark = diags.size();
    if (ir.doors.empty() || !fill.mesh || fill.mesh->pointCount() == 0) return true;
    const auto* styles = styleCol(fill.mesh);
    if (!styles) {
        push(diags, mark, "opening_voids", "mesh without @style (slots §1: v1 assets must set it)");
        return false;
    }
    if (!fill.mesh->cornerVerts || !fill.mesh->faceOffsets) return true;
    const auto& corners = *fill.mesh->cornerVerts;
    const auto& offs = *fill.mesh->faceOffsets;
    for (const auto& d : ir.doors) {
        const double dx = d.to.first - d.from.first, dz = d.to.second - d.from.second;
        const double len = std::hypot(dx, dz);
        const double ux = dx / len, uz = dz / len, vx = -uz, vz = ux;
        const double cx = (d.from.first + d.to.first) * 0.5;
        const double cz = (d.from.second + d.to.second) * 0.5;
        const double halfAxis = d.clear / 2 + d.frame - 1e-4;
        const double halfNormal = d.thick / 2 + 0.05 + 1e-3;
        for (size_t f = 0; f + 1 < offs.size(); ++f) {
            if (offs[f + 1] == offs[f]) continue;
            // Face box in the door frame: 2D (along, y) overlap with the void
            // rect + across-range meeting the void depth (centroids miss wide
            // faces whose middle is elsewhere).
            double a0 = 1e300, a1 = -1e300, y0 = 1e300, y1 = -1e300;
            double b0 = 1e300, b1 = -1e300;
            int64_t worst = 0;
            for (int32_t i = offs[f]; i < offs[f + 1]; ++i) {
                const int pi = corners[i];
                const glm::vec3& p = (*fill.mesh->positions)[pi];
                const double a = (p.x - cx) * ux + (p.z - cz) * uz;
                const double b = (p.x - cx) * vx + (p.z - cz) * vz;
                a0 = std::min(a0, a);
                a1 = std::max(a1, a);
                b0 = std::min(b0, b);
                b1 = std::max(b1, b);
                y0 = std::min(y0, (double)p.y);
                y1 = std::max(y1, (double)p.y);
                if (pi < (int)styles->size()) worst = std::max(worst, (*styles)[pi]);
            }
            const double area2d = std::max(0.0, std::min(a1, halfAxis) - std::max(a0, -halfAxis)) *
                                  std::max(0.0, std::min(y1, d.h - 1e-4) - std::max(y0, 0.0));
            if (area2d > 1e-6 && b0 < halfNormal && b1 > -halfNormal && worst != 0)
                push(diags, mark, "opening_voids",
                     "door " + d.id + ": styled face (style " + std::to_string(worst) +
                         ") inside opening void (" + std::to_string(area2d) + " m²)");
            if (diags.size() >= mark + kCap) return false;
        }
    }
    return diags.size() == mark;
}

bool check_transitions(const IrV2& ir, const Project& project, const FillResult& fill,
                       std::vector<CheckDiag>& diags) {
    const size_t mark = diags.size();
    if (!fill.mesh || fill.mesh->pointCount() == 0) return true;
    const auto* styles = styleCol(fill.mesh);
    if (!styles) {
        push(diags, mark, "transitions", "mesh without @style (slots §1: v1 assets must set it)");
        return false;
    }
    std::map<std::string, const IrFacing*> facings;
    for (const auto& f : ir.facings) facings[f.id] = &f;
    std::map<std::string, const IrNode*> nodes;
    for (const auto& n : ir.nodes) nodes[n.id] = &n;
    const double cell = project.fill.cell;
    for (const auto& span : fill.units) {
        const IrFacing* f = nullptr;
        const IrNode* n = nullptr;
        if (span.slot == "facing") {
            const auto it = facings.find(span.id);
            if (it == facings.end()) continue;
            f = it->second;
            if (f->zones.empty()) continue;
        } else if (span.slot == "node") {
            const auto it = nodes.find(span.id);
            if (it == nodes.end()) continue;
            n = it->second;
        } else {
            continue;
        }
        double ux = 0, uz = 0;
        if (f) {
            const double dx = f->to.first - f->from.first, dz = f->to.second - f->from.second;
            const double len = std::hypot(dx, dz);
            ux = dx / len;
            uz = dz / len;
        }
        const double vx = n ? n->at.first * cell : 0, vz = n ? n->at.second * cell : 0;
        for (const Element& e : groupFaces(fill.mesh, span.meshBegin, span.meshEnd, styles)) {
            if (e.styles.size() != 1) continue;  // check_elements reports mixing
            const int64_t paint = *e.styles.begin();
            double yc = 0;
            for (int pi : e.pts) yc += (*fill.mesh->positions)[pi].y;
            yc /= (double)e.pts.size();
            if (f) {
                double lmin = 1e300, lmax = -1e300;
                for (int pi : e.pts) {
                    const glm::vec3& p = (*fill.mesh->positions)[pi];
                    const double l = (p.x - f->from.first) * ux + (p.z - f->from.second) * uz;
                    lmin = std::min(lmin, l);
                    lmax = std::max(lmax, l);
                }
                const ZonePiece* last = nullptr;
                for (const auto& p : f->zones)
                    if (lmin <= p.l1 && lmax >= p.l0) last = &p;
                if (!last) continue;  // own style, unconstrained
                const double t0 = last->flip == 0 ? last->t_at_l0 + lmin : last->t_at_l0 - lmax;
                const double t1 = last->flip == 0 ? last->t_at_l0 + lmax : last->t_at_l0 - lmin;
                const double b = zoneBoundary(*last, yc);
                if (t0 < b - kTol && t1 > b + kTol) {
                    push(diags, mark, "transitions",
                         span.id + ": element straddles zone " + std::to_string(last->zone) +
                             " boundary (t " + std::to_string(t0) + ".." + std::to_string(t1) +
                             " vs " + std::to_string(b) + ")");
                } else if (paint != last->style_a && paint != last->style_b) {
                    push(diags, mark, "transitions",
                         span.id + ": element painted " + std::to_string(paint) +
                             " inside zone " + std::to_string(last->zone) + " (A=" +
                             std::to_string(last->style_a) + ", B=" + std::to_string(last->style_b) +
                             ")");
                } else if (paint == last->style_a && t0 > b - kTol) {
                    push(diags, mark, "transitions",
                         span.id + ": A-painted element firmly in B territory (zone " +
                             std::to_string(last->zone) + ")");
                } else if (paint == last->style_b && t1 < b + kTol) {
                    push(diags, mark, "transitions",
                         span.id + ": B-painted element firmly in A territory (zone " +
                             std::to_string(last->zone) + ")");
                }
            } else {
                // Attribute to the nearest node face (pillar points match none).
                glm::vec3 cen(0);
                for (int pi : e.pts) cen += (*fill.mesh->positions)[pi];
                cen /= (float)e.pts.size();
                const IrNodeFace* face = nullptr;
                double best = 0.06;
                for (const auto& fc : n->faces) {
                    const double dist = std::abs((cen.x - (vx + fc.center.first)) * fc.n.first +
                                                 (cen.z - (vz + fc.center.second)) * fc.n.second);
                    if (dist < best) {
                        best = dist;
                        face = &fc;
                    }
                }
                if (!face || face->zones.empty()) continue;
                const double rx = face->n.second, rz = -face->n.first;  // right(n)
                const double fx = vx + face->center.first, fz = vz + face->center.second;
                double lmin = 1e300, lmax = -1e300;
                for (int pi : e.pts) {
                    const glm::vec3& p = (*fill.mesh->positions)[pi];
                    const double l = (p.x - fx) * rx + (p.z - fz) * rz;
                    lmin = std::min(lmin, l);
                    lmax = std::max(lmax, l);
                }
                const ZonePiece* last = nullptr;
                for (const auto& p : face->zones)
                    if (lmin <= p.l1 && lmax >= p.l0) last = &p;
                if (!last) continue;
                const double t0 = last->flip == 0 ? last->t_at_l0 + lmin : last->t_at_l0 - lmax;
                const double t1 = last->flip == 0 ? last->t_at_l0 + lmax : last->t_at_l0 - lmin;
                const double b = zoneBoundary(*last, yc);
                if (t0 < b - kTol && t1 > b + kTol)
                    push(diags, mark, "transitions",
                         span.id + ": element straddles zone " + std::to_string(last->zone) +
                             " boundary");
                else if (paint != last->style_a && paint != last->style_b)
                    push(diags, mark, "transitions",
                         span.id + ": element painted " + std::to_string(paint) + " inside zone " +
                             std::to_string(last->zone));
                else if (paint == last->style_a && t0 > b - kTol)
                    push(diags, mark, "transitions",
                         span.id + ": A-painted element firmly in B territory (zone " +
                             std::to_string(last->zone) + ")");
                else if (paint == last->style_b && t1 < b + kTol)
                    push(diags, mark, "transitions",
                         span.id + ": B-painted element firmly in A territory (zone " +
                             std::to_string(last->zone) + ")");
            }
            if (diags.size() >= mark + kCap) return false;
        }
    }
    return diags.size() == mark;
}

bool check_anchors(const IrV2& ir, const Project& project, const FillResult& fill,
                   std::vector<CheckDiag>& diags) {
    const size_t mark = diags.size();
    if (!fill.anchors || fill.anchors->pointCount() == 0) return true;
    const pgg::AttrColumn* kindCol =
        fill.anchors->pointAttrs ? fill.anchors->pointAttrs->find("kind") : nullptr;
    const auto* kinds = kindCol ? std::get_if<std::shared_ptr<const std::vector<int64_t>>>(
                                      &kindCol->data)
                                : nullptr;
    if (!kinds) {
        push(diags, mark, "anchors", "anchors without an int @kind column");
        return false;
    }
    struct Box {
        double x0, x1, y0, y1, z0, z1;
        std::string what;
    };
    std::vector<Box> solids;
    const double cell = project.fill.cell;
    for (const auto& w : ir.walls) {
        const double ax = w.g0.first * cell, az = w.g0.second * cell;
        const double bx = w.g1.first * cell, bz = w.g1.second * cell;
        solids.push_back({std::min(ax, bx) - 1e-3, std::max(ax, bx) + 1e-3, 0,
                          std::max(w.h_left, w.h_right), std::min(az, bz) - 1e-3,
                          std::max(az, bz) + 1e-3, "wall " + w.id});
        // Thickness is applied below (axis-aligned walls in v1).
        Box& b = solids.back();
        if (std::abs(ax - bx) < 1e-9 && std::abs(az - bz) > 1e-9) {
            b.x0 = ax - w.thick / 2 + 1e-3;
            b.x1 = ax + w.thick / 2 - 1e-3;
        } else if (std::abs(az - bz) < 1e-9 && std::abs(ax - bx) > 1e-9) {
            b.z0 = az - w.thick / 2 + 1e-3;
            b.z1 = az + w.thick / 2 - 1e-3;
        } else {
            push(diags, mark, "anchors", "wall " + w.id + " is not axis-aligned");
            solids.pop_back();
        }
    }
    for (const auto& n : ir.nodes) {
        const double x = n.at.first * cell, z = n.at.second * cell;
        solids.push_back({x - n.thick / 2 + 1e-3, x + n.thick / 2 - 1e-3, 0, n.h_pillar,
                          z - n.thick / 2 + 1e-3, z + n.thick / 2 - 1e-3, "node " + n.id});
    }
    for (size_t i = 0; i < fill.anchors->pointCount(); ++i) {
        if (i >= (*kinds)->size() || (**kinds)[i] != 1) continue;  // lights only (F11)
        const glm::vec3& p = (*fill.anchors->positions)[i];
        for (const auto& b : solids)
            if (p.x > b.x0 && p.x < b.x1 && p.y > b.y0 && p.y < b.y1 && p.z > b.z0 && p.z < b.z1)
                push(diags, mark, "anchors",
                     "light anchor inside " + b.what + " at " + pt3(p));
        if (diags.size() >= mark + kCap) return false;
    }
    return diags.size() == mark;
}

bool check_spans(const IrV2& ir, const Project& project, std::vector<CheckDiag>& diags) {
    const size_t mark = diags.size();
    // Bodies: v1 walls are axis-aligned grid atoms. Colinear atoms must be
    // disjoint (T-split); crossings must sit on a node.
    std::map<int, std::vector<const IrWall*>> vert, horiz;  // line coord -> walls
    for (const auto& w : ir.walls) {
        const bool v = w.g0.first == w.g1.first;
        const bool h = w.g0.second == w.g1.second;
        if (!v && !h) {
            push(diags, mark, "spans", "wall " + w.id + " is not axis-aligned");
            continue;
        }
        (v ? vert[w.g0.first] : horiz[w.g0.second]).push_back(&w);
    }
    for (const auto& [coord, vec] : vert)
        for (size_t i = 0; i < vec.size(); ++i)
            for (size_t j = i + 1; j < vec.size(); ++j) {
                const double lo = std::max(std::min(vec[i]->g0.second, vec[i]->g1.second),
                                           std::min(vec[j]->g0.second, vec[j]->g1.second));
                const double hi = std::min(std::max(vec[i]->g0.second, vec[i]->g1.second),
                                           std::max(vec[j]->g0.second, vec[j]->g1.second));
                if (hi - lo > 1e-9)
                    push(diags, mark, "spans",
                         vec[i]->id + " overlaps " + vec[j]->id + " on x=" +
                             std::to_string(coord));
            }
    for (const auto& [coord, vec] : horiz)
        for (size_t i = 0; i < vec.size(); ++i)
            for (size_t j = i + 1; j < vec.size(); ++j) {
                const double lo = std::max(std::min(vec[i]->g0.first, vec[i]->g1.first),
                                           std::min(vec[j]->g0.first, vec[j]->g1.first));
                const double hi = std::min(std::max(vec[i]->g0.first, vec[i]->g1.first),
                                           std::max(vec[j]->g0.first, vec[j]->g1.first));
                if (hi - lo > 1e-9)
                    push(diags, mark, "spans",
                         vec[i]->id + " overlaps " + vec[j]->id + " on y=" +
                             std::to_string(coord));
            }
    std::set<GridPt> nodeAt;
    for (const auto& n : ir.nodes) nodeAt.insert(n.at);
    for (const auto& [x, vv] : vert)
        for (const auto& [y, hh] : horiz)
            for (const IrWall* a : vv)
                for (const IrWall* b : hh) {
                    const double alo = std::min(a->g0.second, a->g1.second);
                    const double ahi = std::max(a->g0.second, a->g1.second);
                    const double blo = std::min(b->g0.first, b->g1.first);
                    const double bhi = std::max(b->g0.first, b->g1.first);
                    if (y > alo && y < ahi && x > blo && x < bhi &&
                        !nodeAt.count(GridPt{x, y}))
                        push(diags, mark, "spans",
                             a->id + " crosses " + b->id + " at (" + std::to_string(x) + "," +
                                 std::to_string(y) + ") with no node");
    // T-junctions: a wall endpoint strictly inside a perpendicular wall's
    // span must sit on a node (grid coords: exact compares).
    for (const auto& [x, vv] : vert)
        for (const IrWall* a : vv) {
            const int alo = std::min(a->g0.second, a->g1.second);
            const int ahi = std::max(a->g0.second, a->g1.second);
            for (const int ey : {alo, ahi})
                if (horiz.count(ey))
                    for (const IrWall* b : horiz.at(ey)) {
                        const int blo = std::min(b->g0.first, b->g1.first);
                        const int bhi = std::max(b->g0.first, b->g1.first);
                        if (x > blo && x < bhi && !nodeAt.count(GridPt{x, ey}))
                            push(diags, mark, "spans",
                                 a->id + " ends inside " + b->id + " at (" + std::to_string(x) +
                                     "," + std::to_string(ey) + ") with no node");
                    }
        }
    for (const auto& [y, hh] : horiz)
        for (const IrWall* b : hh) {
            const int blo = std::min(b->g0.first, b->g1.first);
            const int bhi = std::max(b->g0.first, b->g1.first);
            for (const int ex : {blo, bhi})
                if (vert.count(ex))
                    for (const IrWall* a : vert.at(ex)) {
                        const int alo = std::min(a->g0.second, a->g1.second);
                        const int ahi = std::max(a->g0.second, a->g1.second);
                        if (y > alo && y < ahi && !nodeAt.count(GridPt{ex, y}))
                            push(diags, mark, "spans",
                                 b->id + " ends inside " + a->id + " at (" + std::to_string(ex) +
                                     "," + std::to_string(y) + ") with no node");
                    }
        }
                }
    // Facings per room: development intervals must not overlap (adjacent
    // atoms touch across pillar/T gaps, never over them).
    std::map<std::string, std::vector<const IrFacing*>> perRoom;
    for (const auto& f : ir.facings) perRoom[f.room].push_back(&f);
    for (const auto& [room, vec] : perRoom)
        for (size_t i = 0; i < vec.size(); ++i)
            for (size_t j = i + 1; j < vec.size(); ++j)
                if (std::min(vec[i]->s1, vec[j]->s1) - std::max(vec[i]->s0, vec[j]->s0) > 1e-9)
                    push(diags, mark, "spans",
                         vec[i]->id + " overlaps " + vec[j]->id + " in room " + room);
    // Pillars: distinct grid vertices are disjoint by construction; assert it.
    if (nodeAt.size() != ir.nodes.size())
        push(diags, mark, "spans", "two nodes share one grid vertex");
    // Facings clear of pillar interiors (ends touch pillar faces: fine).
    // v1 facings are axis-aligned plan segments: penetration = positive
    // overlap along the run with the line strictly inside the cross range.
    const double cell = project.fill.cell;
    for (const auto& f : ir.facings) {
        const double fx0 = std::min(f.from.first, f.to.first);
        const double fx1 = std::max(f.from.first, f.to.first);
        const double fz0 = std::min(f.from.second, f.to.second);
        const double fz1 = std::max(f.from.second, f.to.second);
        for (const auto& n : ir.nodes) {
            const double px = n.at.first * cell, pz = n.at.second * cell;
            const double h = n.thick / 2 - 1e-6;  // strict interior
            const bool alongX = (fx1 - fx0) >= (fz1 - fz0);
            bool inside = false;
            if (alongX)
                inside = fz0 > pz - h && fz0 < pz + h &&
                         std::min(fx1, px + h) - std::max(fx0, px - h) > 1e-9;
            else
                inside = fx0 > px - h && fx0 < px + h &&
                         std::min(fz1, pz + h) - std::max(fz0, pz - h) > 1e-9;
            if (inside) push(diags, mark, "spans", f.id + " runs through " + n.id);
        }
    }
    return diags.size() == mark;
}

namespace {

// Per-span elements logic shared by check_elements / check_units /
// check_elements_cached. Messages are bare suffixes (no unit id): callers
// prefix "F11/elements: <span.id>: " via push().
void checkSpanElements(const pgg::GeoPtr& mesh, size_t begin, size_t end,
                       const std::vector<int64_t>* styles,
                       std::vector<std::string>& msgs) {
    const auto elems = groupFaces(mesh, begin, end, styles);
    for (const auto& e : elems)
        if (e.styles.size() != 1)
            msgs.push_back("element with " + std::to_string(e.styles.size()) +
                           " styles (mixed paint)");
    const std::vector<FaceInfo> faces = spanFaces(mesh, begin, end);
    for (size_t i = 0; i < faces.size(); ++i)
        for (size_t j = i + 1; j < faces.size(); ++j)
            if (doubleArea(faces[i], faces[j], *mesh->positions) > 1e-6)
                msgs.push_back("coincident faces (double geometry) at " + pt3(faces[i].c));
}

void pushSpanMsgs(std::vector<CheckDiag>& diags, size_t mark, const std::string& unitId,
                  const std::vector<std::string>& msgs) {
    for (const std::string& m : msgs) push(diags, mark, "elements", unitId + ": " + m);
}

}  // namespace

bool check_elements(const FillResult& fill, std::vector<CheckDiag>& diags) {
    const size_t mark = diags.size();
    if (!fill.mesh || fill.mesh->pointCount() == 0) return true;
    const auto* styles = styleCol(fill.mesh);
    if (!styles) {
        push(diags, mark, "elements", "mesh without @style (slots §1: v1 assets must set it)");
        return false;
    }
    for (const auto& span : fill.units) {
        std::vector<std::string> msgs;
        checkSpanElements(fill.mesh, span.meshBegin, span.meshEnd, styles, msgs);
        pushSpanMsgs(diags, mark, span.id, msgs);
        if (diags.size() >= mark + kCap) return false;
    }
    return diags.size() == mark;
}

bool check_elements_cached(const FillResult& fill, UnitCache* cache,
                           std::vector<CheckDiag>& diags) {
    if (!cache) return check_elements(fill, diags);
    const size_t mark = diags.size();
    if (!fill.mesh || fill.mesh->pointCount() == 0) return true;
    const auto* styles = styleCol(fill.mesh);
    if (!styles) {
        push(diags, mark, "elements", "mesh without @style (slots §1: v1 assets must set it)");
        return false;
    }
    for (const auto& span : fill.units) {
        UnitCache::CheckVerdict v;
        const UnitKey key{span.cacheKey};
        if (span.cacheKey != 0 && cache->lookupCheck(key, kElementsCheckVersion, v)) {
            pushSpanMsgs(diags, mark, span.id, v.messages);
        } else {
            std::vector<std::string> msgs;
            checkSpanElements(fill.mesh, span.meshBegin, span.meshEnd, styles, msgs);
            if (span.cacheKey != 0)
                cache->storeCheck(key,
                                  UnitCache::CheckVerdict{kElementsCheckVersion, msgs});
            pushSpanMsgs(diags, mark, span.id, msgs);
        }
        if (diags.size() >= mark + kCap) return false;
    }
    return diags.size() == mark;
}

bool check_units(const FillResult& fill, const std::string& unit_substr,
                 std::vector<CheckDiag>& diags, size_t* matched_out) {
    const size_t mark = diags.size();
    size_t matched = 0;
    const bool hasMesh = fill.mesh && fill.mesh->pointCount() > 0;
    const auto* styles = hasMesh ? styleCol(fill.mesh) : nullptr;
    bool styleReported = false;
    for (const auto& span : fill.units) {
        if (span.id.find(unit_substr) == std::string::npos) continue;
        ++matched;
        if (!hasMesh) continue;
        if (!styles) {
            if (!styleReported) {
                styleReported = true;
                push(diags, mark, "elements",
                     "mesh without @style (slots §1: v1 assets must set it)");
            }
            continue;
        }
        std::vector<std::string> msgs;
        checkSpanElements(fill.mesh, span.meshBegin, span.meshEnd, styles, msgs);
        pushSpanMsgs(diags, mark, span.id, msgs);
        if (diags.size() >= mark + kCap) break;
    }
    if (matched_out) *matched_out = matched;
    if (matched == 0) {
        push(diags, mark, "elements",
             "unit filter \"" + unit_substr + "\" matches no unit (see the units listing)");
        return false;
    }
    return diags.size() == mark;
}

// --- facing bounds -----------------------------------------------------------

namespace {

bool ptInPolyXZ(const std::vector<std::pair<double, double>>& c, double x, double z) {
    bool inside = false;
    const size_t n = c.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const double xi = c[i].first, zi = c[i].second;
        const double xj = c[j].first, zj = c[j].second;
        if ((zi > z) != (zj > z) && x < (xj - xi) * (z - zi) / (zj - zi) + xi) inside = !inside;
    }
    return inside;
}

double distToPolyXZ(const std::vector<std::pair<double, double>>& c, double x, double z) {
    double best = 1e300;
    const size_t n = c.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const double ax = c[j].first, az = c[j].second;
        const double bx = c[i].first, bz = c[i].second;
        const double dx = bx - ax, dz = bz - az;
        const double len2 = dx * dx + dz * dz;
        const double t = len2 > 0 ? std::max(0.0, std::min(1.0, ((x - ax) * dx + (z - az) * dz) / len2))
                                  : 0.0;
        const double ex = ax + t * dx - x, ez = az + t * dz - z;
        best = std::min(best, std::hypot(ex, ez));
    }
    return best;
}

}  // namespace

bool check_facing_bounds(const IrV2& ir, const Project& project, const FillResult& fill,
                         std::vector<CheckDiag>& diags) {
    const size_t mark = diags.size();
    if (!fill.mesh || fill.mesh->pointCount() == 0) return true;
    std::map<std::string, const IrFacing*> facings;
    for (const auto& f : ir.facings) facings[f.id] = &f;
    std::map<std::string, const IrRoom*> rooms;
    for (const auto& r : ir.rooms) rooms[r.id] = &r;
    const double cell = project.fill.cell;
    for (const auto& span : fill.units) {
        if (span.slot != "facing") continue;
        const auto fit = facings.find(span.id);
        if (fit == facings.end()) continue;
        const IrRoom* room = rooms[fit->second->room];
        if (!room) continue;
        std::vector<std::pair<double, double>> contour;
        for (const auto& [gx, gy] : room->grid) contour.push_back({gx * cell, gy * cell});
        for (size_t i = span.meshBegin; i < span.meshEnd; ++i) {
            const glm::vec3& p = (*fill.mesh->positions)[i];
            if (ptInPolyXZ(contour, p.x, p.z)) continue;
            if (distToPolyXZ(contour, p.x, p.z) > 1e-4)
                push(diags, mark, "facing_bounds",
                     span.id + ": point (" + std::to_string(p.x) + ", " + std::to_string(p.z) +
                         ") is outside room " + room->id +
                         " (facing must hug the room's side of the wall, 5.2)");
            if (diags.size() >= mark + kCap) return false;
        }
    }
    return diags.size() == mark;
}

bool check_level(const IrV2& ir, const Project& project, const FillResult& fill,
                 std::vector<CheckDiag>& diags) {
    bool ok = true;
    ok = check_passage(ir, project, diags) && ok;
    ok = check_opening_voids(ir, fill, diags) && ok;
    ok = check_transitions(ir, project, fill, diags) && ok;
    ok = check_anchors(ir, project, fill, diags) && ok;
    ok = check_spans(ir, project, diags) && ok;
    ok = check_elements(fill, diags) && ok;
    ok = check_facing_bounds(ir, project, fill, diags) && ok;
    return ok;
}

bool check_level_cached(const IrV2& ir, const Project& project, const FillResult& fill,
                        UnitCache* cache, std::vector<CheckDiag>& diags) {
    bool ok = true;
    ok = check_passage(ir, project, diags) && ok;
    ok = check_opening_voids(ir, fill, diags) && ok;
    ok = check_transitions(ir, project, fill, diags) && ok;
    ok = check_anchors(ir, project, fill, diags) && ok;
    ok = check_spans(ir, project, diags) && ok;
    ok = check_elements_cached(fill, cache, diags) && ok;
    ok = check_facing_bounds(ir, project, fill, diags) && ok;
    return ok;
}

}  // namespace delve
