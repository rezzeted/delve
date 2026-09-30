// Delve R-A3 (slots §4): static slot-asset checks without running PGG.

#include "fill.h"

#include <filesystem>
#include <map>

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

}  // namespace

bool check_asset(const std::string& slot, const std::string& asset_path,
                 const std::vector<std::string>& import_roots,
                 std::vector<SlotDiag>& diags) {
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
        if (!required && !info.hasDefault)
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

}  // namespace delve
