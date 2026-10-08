// Delve R-A3 (slots §4): static slot-asset checks without running PGG;
// plus the A2 contract lint (one synthetic run + strict output-schema check).

#include "fill.h"

#include <filesystem>
#include <map>
#include <set>

#include <glm/glm.hpp>

#include "pgg/eval.h"
#include "pgg/pgg.h"
#include "pgg/src/ast.h"
#include "pgg/src/eval/expand.h"
#include "pgg/src/eval/modules.h"
#include "pgg/src/eval/typecheck.h"

namespace delve {
namespace {

struct ParamSpec {
    const char* name;
    const char* base;     // f32|int|bool|vec3|geo
    const char* geoKind;  // points (geo params) or ""
};

// Slots §2 param tables (required params; extra params need defaults).
const std::map<std::string, std::vector<ParamSpec>> kContracts = {
    {"room_fill",
     {{"contour", "geo", "points"},
      {"h", "f32", ""},
      {"role", "int", ""},
      {"style_floor", "int", ""},
      {"style_ceil", "int", ""},
      {"rng_seed", "int", ""}}},
    {"wall_body",
     {{"seg", "geo", "points"},
      {"thick", "f32", ""},
      {"h_a", "f32", ""},
      {"h_b", "f32", ""},
      {"style", "int", ""},
      {"cuts", "geo", "points"},
      {"rng_seed", "int", ""}}},
    {"facing",
     {{"seg", "geo", "points"},
      {"n", "vec3", ""},
      {"h", "f32", ""},
      {"style", "int", ""},
      {"module", "f32", ""},
      {"cuts", "geo", "points"},
      {"zones", "geo", "points"},
      {"rng_seed", "int", ""}}},
    {"node",
     {{"faces", "geo", "points"},
      {"thick", "f32", ""},
      {"h_pillar", "f32", ""},
      {"style", "int", ""},
      {"module", "f32", ""},
      {"zones", "geo", "points"},
      {"rng_seed", "int", ""}}},
    {"door",
     {{"seg", "geo", "points"},
      {"h", "f32", ""},
      {"frame", "f32", ""},
      {"thick", "f32", ""},
      {"dtype", "int", ""},
      {"rng_seed", "int", ""}}},
    {"decor",
     {{"p", "geo", "points"},
      {"style", "int", ""},
      {"tag", "int", ""},
      {"rng_seed", "int", ""}}},
};

std::string typeText(const std::string& base, const std::string& geoKind) {
    return base == "geo" ? "geo<" + geoKind + ">" : base;
}

std::string pggDiagText(const std::string& path, const pgg::Diagnostic& d) {
    std::string t = path + ":" + std::to_string(d.span.line) + ":" +
                    std::to_string(d.span.col) + ": " + d.message;
    if (!d.hint.empty()) t += " [" + d.hint + "]";
    return t;
}

bool diagsHaveErrors(const std::vector<SlotDiag>& diags) {
    for (const auto& d : diags)
        if (!d.warning) return true;
    return false;
}

// --- contract lint (A2): synthetic inputs + output schema -------------------

// Minimal geo<points> builder (a fill.cpp PointsBuilder without the error
// channel: the lint fixtures are consistent by construction).
struct SynthPoints {
    std::vector<glm::vec3> pos;
    std::map<std::string, std::vector<float>> f32c;
    std::map<std::string, std::vector<int64_t>> intc;
    std::map<std::string, std::vector<uint8_t>> boolc;
    std::map<std::string, std::vector<glm::vec3>> vec3c;

    void pt(float x, float y, float z) { pos.emplace_back(x, y, z); }
    void f32(const std::string& name, float v) { f32c[name].push_back(v); }
    void integer(const std::string& name, int64_t v) { intc[name].push_back(v); }
    void boolean(const std::string& name, bool v) { boolc[name].push_back(v ? 1 : 0); }
    void vec3(const std::string& name, float x, float y, float z) {
        vec3c[name].emplace_back(x, y, z);
    }
    pgg::GeoPtr build() const {
        auto attrs = std::make_shared<pgg::AttrSet>();
        for (const auto& [name, col] : f32c)
            attrs->columns[name] =
                pgg::AttrColumn{std::make_shared<const std::vector<float>>(col)};
        for (const auto& [name, col] : intc)
            attrs->columns[name] =
                pgg::AttrColumn{std::make_shared<const std::vector<int64_t>>(col)};
        for (const auto& [name, col] : boolc)
            attrs->columns[name] =
                pgg::AttrColumn{std::make_shared<const std::vector<uint8_t>>(col)};
        for (const auto& [name, col] : vec3c)
            attrs->columns[name] =
                pgg::AttrColumn{std::make_shared<const std::vector<glm::vec3>>(col)};
        auto g = std::make_shared<pgg::Geo>();
        g->kind = pgg::GeoKind::Points;
        g->positions = std::make_shared<const std::vector<glm::vec3>>(pos);
        g->pointAttrs = std::move(attrs);
        return g;
    }
};

pgg::Value synthSeg() {  // 2-point run, 2.4 m along +X (like expand* in fill.cpp)
    SynthPoints seg;
    seg.pt(0, 0, 0);
    seg.pt(2.4f, 0, 0);
    return pgg::Value(seg.build());
}

// Minimal plausible bindings per slot kind, mirroring the expand* fixtures.
std::vector<std::pair<std::string, pgg::Value>> synthBindings(const std::string& kind) {
    const pgg::Value seed(static_cast<int64_t>(7));
    if (kind == "room_fill") {
        SynthPoints c;  // 3.2 x 3.2 square, CCW, all corners convex
        const float k = 3.2f;
        for (const auto& [x, z] : {std::pair{0.0f, 0.0f}, {k, 0.0f}, {k, k}, {0.0f, k}}) {
            c.pt(x, 0, z);
            c.f32("edge_len", k);
            c.boolean("convex", true);
        }
        return {{"contour", pgg::Value(c.build())},
                {"h", pgg::Value(3.0f)},
                {"role", pgg::Value(static_cast<int64_t>(1))},
                {"style_floor", pgg::Value(static_cast<int64_t>(1))},
                {"style_ceil", pgg::Value(static_cast<int64_t>(3))},
                {"rng_seed", seed}};
    }
    if (kind == "wall_body")
        return {{"seg", synthSeg()},       {"thick", pgg::Value(0.8f)},
                {"h_a", pgg::Value(3.0f)}, {"h_b", pgg::Value(3.0f)},
                {"style", pgg::Value(static_cast<int64_t>(1))},
                {"cuts", pgg::Value(SynthPoints{}.build())},
                {"rng_seed", seed}};
    if (kind == "facing")
        return {{"seg", synthSeg()},
                {"n", pgg::Value(glm::vec3(0, 0, 1))},
                {"h", pgg::Value(3.0f)},
                {"style", pgg::Value(static_cast<int64_t>(1))},
                {"module", pgg::Value(0.8f)},
                {"cuts", pgg::Value(SynthPoints{}.build())},
                {"zones", pgg::Value(SynthPoints{}.build())},
                {"rng_seed", seed}};
    if (kind == "node") {
        SynthPoints faces;  // 4 open faces of a 0.8-thick pillar, normals outward
        const float t = 0.4f;
        const glm::vec3 ns[4] = {{1, 0, 0}, {0, 0, 1}, {-1, 0, 0}, {0, 0, -1}};
        const glm::vec3 ps[4] = {{t, 0, 0}, {0, 0, t}, {-t, 0, 0}, {0, 0, -t}};
        for (int i = 0; i < 4; ++i) {
            faces.pt(ps[i].x, ps[i].y, ps[i].z);
            faces.vec3("n", ns[i].x, ns[i].y, ns[i].z);
            faces.f32("h", 3.0f);
            faces.integer("style", 1);
            faces.integer("face", i);
        }
        return {{"faces", pgg::Value(faces.build())},
                {"thick", pgg::Value(0.8f)},
                {"h_pillar", pgg::Value(3.2f)},
                {"style", pgg::Value(static_cast<int64_t>(1))},
                {"module", pgg::Value(0.8f)},
                {"zones", pgg::Value(SynthPoints{}.build())},
                {"rng_seed", seed}};
    }
    if (kind == "door")
        return {{"seg", synthSeg()},       {"h", pgg::Value(2.2f)},
                {"frame", pgg::Value(0.12f)}, {"thick", pgg::Value(0.8f)},
                {"dtype", pgg::Value(static_cast<int64_t>(1))},
                {"rng_seed", seed}};
    // decor (and decor:<tag>): one floor point, normal up
    SynthPoints p;
    p.pt(0, 0, 0);
    p.vec3("n", 0, 1, 0);
    return {{"p", pgg::Value(p.build())},
            {"style", pgg::Value(static_cast<int64_t>(1))},
            {"tag", pgg::Value(static_cast<int64_t>(1))},
            {"rng_seed", seed}};
}

const char* colTypeName(size_t tag) {
    switch (tag) {
        case 0: return "f32";
        case 1: return "int";
        case 2: return "bool";
        case 3: return "vec2";
        case 4: return "vec3";
        case 5: return "vec4";
        default: return "string";
    }
}

// Assembly-merge restrictions (fill.cpp mergeGeos/mergeDomain): no groups,
// no detail attrs, no instances, no vec2/vec4 columns.
void lintMergeRules(const pgg::GeoPtr& g, const char* outName, const std::string& where,
                    std::vector<SlotDiag>& diags) {
    const std::pair<const char*, const pgg::GroupSet*> groupSets[4] = {
        {"points", g->pointGroups.get()},
        {"corners", g->cornerGroups.get()},
        {"faces", g->faceGroups.get()},
        {"detail", g->detailGroups.get()}};
    for (const auto& [domain, set] : groupSets) {
        if (!set) continue;
        for (const auto& [name, col] : set->columns)
            diags.push_back({"delve/lint",
                             where + ": output '" + outName + "' keeps group '" + name +
                                 "' on " + domain +
                                 " (the assembly merge rejects groups: unmark before output)"});
    }
    if (g->detailAttrs)
        for (const auto& [name, col] : g->detailAttrs->columns)
            diags.push_back({"delve/lint", where + ": output '" + outName +
                                               "' keeps detail attr '@" + name +
                                               "' (the assembly merge rejects detail attrs)"});
    if (g->instanceSources)
        diags.push_back({"delve/lint", where + ": output '" + outName +
                                           "' keeps instance sources (realize before output)"});
    const std::pair<const char*, const pgg::AttrSet*> attrSets[3] = {
        {"points", g->pointAttrs.get()},
        {"corners", g->cornerAttrs.get()},
        {"faces", g->faceAttrs.get()}};
    for (const auto& [domain, set] : attrSets) {
        if (!set) continue;
        for (const auto& [name, col] : set->columns)
            if (col.data.index() == 3 || col.data.index() == 5)
                diags.push_back({"delve/lint",
                                 where + ": output '" + outName + "' column '@" + name + "' on " +
                                     domain + " is " + colTypeName(col.data.index()) +
                                     " (the assembly merge supports f32/int/bool/vec3/string: "
                                     "remove_attr before output)"});
    }
}

const pgg::AttrColumn* pointCol(const pgg::GeoPtr& g, const char* name) {
    return g->pointAttrs ? g->pointAttrs->find(name) : nullptr;
}

}  // namespace

bool check_asset(const std::string& slot, const std::string& asset_path,
                 const std::vector<std::string>& import_roots,
                 std::vector<SlotDiag>& diags,
                 std::map<std::string, DeclaredParam>* declared_params) {
    // decor:<tag> shares the decor contract (slots §2.6: tag is a param).
    std::string kind = slot;
    if (kind.rfind("decor:", 0) == 0) kind = "decor";
    const auto contract = kContracts.find(kind);
    if (contract == kContracts.end()) {
        diags.push_back({"delve/slot", "unknown slot kind '" + slot + "' (expected one of " +
                                          "room_fill, wall_body, facing, node, door, decor:<tag>)"});
        return false;
    }
    const std::string where = "slot '" + slot + "' (" + asset_path + ")";

    const pgg::Document doc = pgg::parseFile(asset_path);
    for (const auto& d : doc.diagnostics)
        diags.push_back({d.code, pggDiagText(asset_path, d), d.isWarning});
    if (!doc.file || doc.hasErrors()) return false;

    // Interface: params, outputs, slot_version.
    struct ParamInfo {
        std::string base, geoKind;
        bool hasDefault = false;
    };
    std::map<std::string, ParamInfo> params;
    std::vector<std::string> outputs;
    const pgg::Def* slot_version = nullptr;
    for (const pgg::Node* item : doc.file->items) {
        if (item->kind == pgg::NodeKind::ParamDecl) {
            const auto* p = static_cast<const pgg::ParamDecl*>(item);
            ParamInfo info;
            if (p->type) {
                info.base = p->type->base;
                info.geoKind = p->type->geoKind;
            }
            info.hasDefault = p->hasDefault;
            params[p->name] = info;
            if (declared_params)
                (*declared_params)[p->name] = {info.base, info.geoKind, info.hasDefault};
        } else if (item->kind == pgg::NodeKind::OutputDecl) {
            outputs.push_back(static_cast<const pgg::OutputDecl*>(item)->name);
        } else if (item->kind == pgg::NodeKind::Def &&
                   static_cast<const pgg::Def*>(item)->name == "slot_version") {
            slot_version = static_cast<const pgg::Def*>(item);
        }
    }

    // Static stage (the pgg::run prefix: imports -> expansion -> typecheck).
    {
        std::vector<std::string> roots;
        const std::string dir = std::filesystem::path(asset_path).parent_path().string();
        if (!dir.empty()) roots.push_back(dir);
        for (const auto& r : import_roots) roots.push_back(r);
        pgg::appendImportRoot(roots, pgg::findProductLibRoot(asset_path));
        std::vector<pgg::Diagnostic> stage;
        pgg::ModuleClosure closure;
        const pgg::ModuleClosure* closurePtr = nullptr;
        if (pgg::hasImports(*doc.file)) {
            closure = pgg::loadModuleClosure(*doc.file, roots, stage);
            closurePtr = &closure;
        }
        pgg::FlatProgram flat = pgg::expandProgram(*doc.file, closurePtr, stage);
        bool errors = false;
        for (const auto& d : stage) errors = errors || !d.isWarning;
        if (!errors) {
            std::vector<std::string> bound;
            for (const auto& [name, info] : params) bound.push_back(name);
            std::vector<size_t> runtimeContracts;
            pgg::typecheckFlat(flat, bound, stage, runtimeContracts);
        }
        for (const auto& d : stage)
            diags.push_back({d.code, pggDiagText(asset_path, d), d.isWarning});
        if (diagsHaveErrors(diags)) return false;
    }

    // Contract compare (slots §4 steps 2-5).
    for (const ParamSpec& want : contract->second) {
        const auto got = params.find(want.name);
        if (got == params.end()) {
            diags.push_back({"delve/slot", where + ": missing param '" + want.name + "' (" +
                                              typeText(want.base, want.geoKind) +
                                              "); fix the asset signature"});
            continue;
        }
        if (got->second.base != want.base || got->second.geoKind != want.geoKind) {
            diags.push_back(
                {"delve/slot", where + ": param '" + want.name + "': expected " +
                                   typeText(want.base, want.geoKind) + ", got " +
                                   typeText(got->second.base, got->second.geoKind)});
        }
        // Slots §5 (R-A4): non-geo params need defaults (geo comes from fixtures).
        if (std::string(want.base) != "geo" && !got->second.hasDefault)
            diags.push_back({"delve/slot", where + ": param '" + want.name +
                                              "' needs a default (R-A4 autonomy)"});
    }
    const auto hasOutput = [&](const std::string& name) {
        for (const auto& o : outputs)
            if (o == name) return true;
        return false;
    };
    // Output geo-kinds (mesh / points) are asserted at the first run: PGG
    // does not expose inferred binding types (slots §4, D1.3 note).
    for (const char* name : {"mesh", "anchors"})
        if (!hasOutput(name))
            diags.push_back({"delve/slot", where + ": missing output '" + name + "'"});
    for (const auto& [name, info] : params) {
        bool required = false;
        for (const ParamSpec& want : contract->second)
            if (want.name == name) required = true;
        // Extra geo inputs are host-bound stream data (like the contract geo
        // params: slots §5 "geo comes from fixtures") — room_fill `cuts` (D5);
        // non-geo extras still need a default (R-A2 autonomy).
        if (!required && !info.hasDefault && info.base != "geo")
            diags.push_back({"delve/slot", where + ": extra param '" + name +
                                              "' needs a default (R-A2)"});
    }
    if (!slot_version) {
        diags.push_back({"delve/slot", where + ": def slot_version() is missing (R-A8)"});
    } else {
        bool ok = slot_version->params.empty() && slot_version->outputs.size() == 1 &&
                  slot_version->outputs[0].name == "out" &&
                  slot_version->outputs[0].type != nullptr &&
                  slot_version->outputs[0].type->base == "int" && slot_version->body.size() == 1 &&
                  slot_version->body[0]->kind == pgg::NodeKind::Binding;
        int version = -1;
        if (ok) {
            const auto* b = static_cast<const pgg::Binding*>(slot_version->body[0]);
            const pgg::Expr* v = b->value;
            ok = b->targets.names.size() == 1 && b->targets.names[0] == "out" && v != nullptr &&
                 v->kind == pgg::NodeKind::NumberLit &&
                 !static_cast<const pgg::NumberLit*>(v)->isFloat;
            if (ok) version = std::stoi(static_cast<const pgg::NumberLit*>(v)->text);
        }
        if (!ok)
            diags.push_back({"delve/slot", where + ": def slot_version() must be exactly " +
                                              "'def slot_version() -> (out: int) { out = <int> }'"});
        else if (version != 1)
            diags.push_back({"delve/slot", where + ": slot_version() is " +
                                              std::to_string(version) + ", required 1"});
    }
    return !diagsHaveErrors(diags);
}

bool lint_asset(const std::string& slot, const std::string& asset_path,
                const std::vector<std::string>& import_roots,
                std::vector<SlotDiag>& diags) {
    std::string kind = slot;
    if (kind.rfind("decor:", 0) == 0) kind = "decor";
    if (kContracts.find(kind) == kContracts.end()) {
        diags.push_back({"delve/slot", "unknown slot kind '" + slot + "' (expected one of " +
                                          "room_fill, wall_body, facing, node, door, decor:<tag>)"});
        return false;
    }
    const std::string where = "slot '" + slot + "' (" + asset_path + ")";

    std::vector<std::string> roots;
    const std::string dir = std::filesystem::path(asset_path).parent_path().string();
    if (!dir.empty()) roots.push_back(dir);
    for (const auto& r : import_roots) roots.push_back(r);
    pgg::appendImportRoot(roots, pgg::findProductLibRoot(asset_path));

    pgg::RunParams rp;
    rp.values = synthBindings(kind);
    // Extra geo inputs without defaults (room_fill `cuts`, D5) are host-bound
    // stream data: the synthetic run binds them empty so the output lint
    // stays conclusive.
    {
        const pgg::Document doc = pgg::parseFile(asset_path);
        if (doc.file && !doc.hasErrors()) {
            std::set<std::string> bound;
            for (const auto& [name, v] : rp.values) bound.insert(name);
            for (const pgg::Node* item : doc.file->items) {
                if (item->kind != pgg::NodeKind::ParamDecl) continue;
                const auto* p = static_cast<const pgg::ParamDecl*>(item);
                if (p->hasDefault || bound.count(p->name) != 0 || !p->type ||
                    p->type->base != "geo")
                    continue;
                auto g = std::make_shared<pgg::Geo>();
                g->kind = p->type->geoKind == "mesh" ? pgg::GeoKind::Mesh
                                                     : pgg::GeoKind::Points;
                g->positions = std::make_shared<const std::vector<glm::vec3>>();
                g->pointAttrs = std::make_shared<pgg::AttrSet>();
                rp.values.emplace_back(p->name, pgg::Value(g));
                bound.insert(p->name);
            }
        }
    }
    rp.importRoots = roots;
    const pgg::RunResult r = pgg::runFile(asset_path, rp);
    if (r.hasErrors()) {
        std::string first;
        for (const auto& d : r.diagnostics)
            if (!d.isWarning) {
                first = "[" + d.code + "] " + d.message;
                break;
            }
        diags.push_back({"delve/lint",
                         where + ": the synthetic contract run failed; the output lint is "
                                 "inconclusive (" +
                                 first + ")",
                         true});
        return true;
    }
    const pgg::Value* meshV = nullptr;
    const pgg::Value* anchorsV = nullptr;
    for (const auto& o : r.outputs) {
        if (o.name == "mesh") meshV = &o.value;
        if (o.name == "anchors") anchorsV = &o.value;
    }
    auto kindOf = [](const pgg::Value* v) {
        if (!v || pgg::valueBase(*v) != pgg::ScalarType::Geo) return "non-geo";
        switch (pgg::asGeo(*v)->kind) {
            case pgg::GeoKind::Mesh: return "geo<mesh>";
            case pgg::GeoKind::Points: return "geo<points>";
            default: return "geo<other>";
        }
    };
    if (!meshV || kindOf(meshV) != std::string("geo<mesh>")) {
        diags.push_back({"delve/lint", where + ": output 'mesh' is " + kindOf(meshV) +
                                           ", required geo<mesh>"});
        return false;
    }
    if (!anchorsV || kindOf(anchorsV) != std::string("geo<points>")) {
        diags.push_back({"delve/lint", where + ": output 'anchors' is " + kindOf(anchorsV) +
                                           ", required geo<points>"});
        return false;
    }
    const pgg::GeoPtr mesh = pgg::asGeo(*meshV);
    const pgg::GeoPtr anchors = pgg::asGeo(*anchorsV);
    lintMergeRules(mesh, "mesh", where, diags);
    lintMergeRules(anchors, "anchors", where, diags);

    if (mesh->pointCount() == 0) {
        diags.push_back({"delve/lint",
                         where + ": the synthetic run produced an empty mesh; the @style/@Cd "
                                 "lint is inconclusive",
                         true});
    } else {
        // Slots §1: mesh points carry @style:int (except door) + @Cd:vec3.
        if (kind != "door") {
            const pgg::AttrColumn* style = pointCol(mesh, "style");
            if (!style)
                diags.push_back({"delve/lint", where + ": mesh without @style on points (slots "
                                                       "§1; F11/elements rejects it)"});
            else if (style->data.index() != 1)
                diags.push_back({"delve/lint", where + ": mesh @style is " +
                                                       colTypeName(style->data.index()) +
                                                       ", required int"});
        }
        const pgg::AttrColumn* cd = pointCol(mesh, "Cd");
        if (!cd)
            diags.push_back({"delve/lint", where + ": mesh without @Cd on points (slots §1: "
                                                   "the preview/export palette)"});
        else if (cd->data.index() != 4)
            diags.push_back({"delve/lint", where + ": mesh @Cd is " +
                                                   colTypeName(cd->data.index()) +
                                                   ", required vec3"});
    }
    // labelAnchors (fill.cpp) requires an int @kind on non-empty anchors.
    if (anchors->pointCount() > 0) {
        const pgg::AttrColumn* kcol = pointCol(anchors, "kind");
        if (!kcol)
            diags.push_back({"delve/lint", where + ": anchors without @kind on points (the "
                                                   "assembly labels anchors by it)"});
        else if (kcol->data.index() != 1)
            diags.push_back({"delve/lint", where + ": anchors @kind is " +
                                                   colTypeName(kcol->data.index()) +
                                                   ", required int"});
    }
    return !diagsHaveErrors(diags);
}

}  // namespace delve
