#include "pch.h"

#include "topo.h"

#include <algorithm>
#include <set>

namespace delve {

CellCentroid contour_centroid(const std::vector<CellPt>& c) {
    CellCentroid out;
    if (c.empty()) return out;
    int minx = c[0].first, maxx = c[0].first, miny = c[0].second, maxy = c[0].second;
    for (const auto& p : c) {
        minx = std::min(minx, p.first);
        maxx = std::max(maxx, p.first);
        miny = std::min(miny, p.second);
        maxy = std::max(maxy, p.second);
    }
    const long long area2 = contour_area2(c);
    if (area2 == 0) {  // degenerate: bbox midpoint
        out.ok = false;
        out.x = (minx + maxx) / 2.0;
        out.z = (miny + maxy) / 2.0;
        return out;
    }
    long long sx = 0, sy = 0;
    for (size_t i = 0; i < c.size(); ++i) {
        const auto [x0, y0] = c[i];
        const auto [x1, y1] = c[(i + 1) % c.size()];
        const long long cross = static_cast<long long>(x0) * y1 - static_cast<long long>(x1) * y0;
        sx += static_cast<long long>(x0 + x1) * cross;
        sy += static_cast<long long>(y0 + y1) * cross;
    }
    out.ok = true;
    out.x = static_cast<double>(sx) / (3.0 * static_cast<double>(area2));
    out.z = static_cast<double>(sy) / (3.0 * static_cast<double>(area2));
    return out;
}

void grid_bbox(const std::vector<CellPt>& c, int& minx, int& maxx, int& miny, int& maxy) {
    if (c.empty()) {
        minx = maxx = miny = maxy = 0;
        return;
    }
    minx = maxx = c[0].first;
    miny = maxy = c[0].second;
    for (const auto& p : c) {
        minx = std::min(minx, p.first);
        maxx = std::max(maxx, p.first);
        miny = std::min(miny, p.second);
        maxy = std::max(maxy, p.second);
    }
}

std::string passage_door(const LayoutParams& g, const std::string& a, const std::string& b) {
    for (const auto& p : g.passages)
        if ((p.a == a && p.b == b) || (p.a == b && p.b == a)) return p.door;
    return {};
}

namespace {

const TopoNode* findNode(const std::vector<TopoNode>& nodes, const std::string& id) {
    // nodes are sorted by id (built from a std::set); the graph is small
    // (<= 30 rooms by N5), a linear scan is fine.
    for (const TopoNode& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}

}  // namespace

TopoModel build_topo(const LayoutParams& graph, const LayoutData& layout) {
    TopoModel m;

    std::set<std::string> ids;
    for (const auto& r : graph.rooms) ids.insert(r.id);
    for (const auto& r : layout.rooms) ids.insert(r.id);
    m.nodes.reserve(ids.size());
    for (const auto& id : ids) {  // std::set = sorted by id
        TopoNode n;
        n.id = id;
        for (const auto& r : graph.rooms)
            if (r.id == id) {
                n.role = r.role;
                n.tags = r.tags;
            }
        for (const auto& r : layout.rooms)
            if (r.id == id) {
                n.hasLayout = true;
                n.corridor = r.corridor;
                n.tmpl = r.tmpl;
                n.contour = r.grid;
                const CellCentroid c = contour_centroid(r.grid);
                n.cx = c.x;
                n.cz = c.z;
                grid_bbox(r.grid, n.minx, n.maxx, n.miny, n.maxy);
            }
        m.nodes.push_back(std::move(n));
    }

    for (const auto& p : graph.passages) {
        TopoEdge e;
        e.a = p.a;
        e.b = p.b;
        e.door = p.door;
        if (const TopoNode* na = findNode(m.nodes, p.a); na && na->hasLayout)
            if (const TopoNode* nb = findNode(m.nodes, p.b); nb && nb->hasLayout) {
                e.labelOk = true;
                e.lx = (na->cx + nb->cx) / 2.0;
                e.lz = (na->cz + nb->cz) / 2.0;
            }
        m.edges.push_back(std::move(e));
    }
    std::sort(m.edges.begin(), m.edges.end(), [](const TopoEdge& x, const TopoEdge& y) {
        if (x.a != y.a) return x.a < y.a;
        return x.b < y.b;
    });

    for (const auto& r : layout.rooms)
        for (const auto& d : r.doors) {
            TopoDoor t;
            t.room = r.id;
            t.to = d.to;
            t.g0 = d.g0;
            t.g1 = d.g1;
            t.door = passage_door(graph, r.id, d.to);
            m.doors.push_back(std::move(t));
        }
    std::sort(m.doors.begin(), m.doors.end(), [](const TopoDoor& x, const TopoDoor& y) {
        if (x.room != y.room) return x.room < y.room;
        if (x.to != y.to) return x.to < y.to;
        if (x.g0 != y.g0) return x.g0 < y.g0;
        return x.g1 < y.g1;
    });

    for (const auto& r : graph.rooms) m.roles.push_back(r.role);
    std::sort(m.roles.begin(), m.roles.end());
    m.roles.erase(std::unique(m.roles.begin(), m.roles.end()), m.roles.end());

    return m;
}

}  // namespace delve
