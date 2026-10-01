#include "export.h"

#include <cctype>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "src/eval/geo_file.h"
#include "src/eval/obj_export.h"

namespace delve {

namespace fs = std::filesystem;

namespace {

bool writeText(const std::string& path, const std::string& text, std::string& err) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        err = "cannot write " + path;
        return false;
    }
    out << text;
    if (!out) {
        err = "cannot write " + path;
        return false;
    }
    return true;
}

// Group names become OBJ file names (<dir>/<name>.<group>.obj), so unit ids
// ("room:entry", "wall:<owner>:<edge>") lose every char outside [A-Za-z0-9._-].
std::string sanitizeGroupName(const std::string& id) {
    std::string out;
    out.reserve(id.size());
    for (const unsigned char c : id)
        out += (std::isalnum(c) || c == '.' || c == '_' || c == '-') ? static_cast<char>(c) : '_';
    return out;
}

// Copy of the mesh with one faces group per unit, baked from the point spans:
// a face belongs to the unit when all its corner points are in the span. Seam
// faces stay ungrouped and land in _nogroup.obj on split.
pgg::GeoPtr meshWithUnitGroups(const pgg::Geo& mesh, const FillResult& fill) {
    auto groups = std::make_shared<pgg::GroupSet>(mesh.faceGroups ? *mesh.faceGroups
                                                                  : pgg::GroupSet{});
    const size_t nf = mesh.faceCount();
    for (const FillResult::UnitSpan& u : fill.units) {
        std::string name = sanitizeGroupName(u.id);
        for (int k = 2; groups->columns.count(name) > 0; ++k)
            name = sanitizeGroupName(u.id) + "_" + std::to_string(k);
        auto col = std::make_shared<pgg::BoolColumn>(nf, 0);
        for (size_t f = 0; f < nf; ++f) {
            bool all = true;
            for (int32_t c = (*mesh.faceOffsets)[f]; c < (*mesh.faceOffsets)[f + 1]; ++c) {
                const size_t p = static_cast<size_t>((*mesh.cornerVerts)[c]);
                if (p < u.meshBegin || p >= u.meshEnd) {
                    all = false;
                    break;
                }
            }
            if (all) (*col)[f] = 1;
        }
        groups->columns[std::move(name)] = std::move(col);
    }
    return pgg::withGroups(mesh, pgg::Domain::Faces, std::move(groups));
}

}  // namespace

bool write_units_json(const FillResult& fill, const std::string& path, std::string& err) {
    nlohmann::ordered_json units = nlohmann::ordered_json::array();
    for (const FillResult::UnitSpan& u : fill.units)
        units.push_back({{"id", u.id},
                         {"slot", u.slot},
                         {"mesh_begin", u.meshBegin},
                         {"mesh_end", u.meshEnd},
                         {"anchors_begin", u.anchorsBegin},
                         {"anchors_end", u.anchorsEnd}});
    const nlohmann::ordered_json doc{{"format", kUnitsFormat}, {"units", std::move(units)}};
    return writeText(path, doc.dump(2) + '\n', err);
}

bool export_level(const IrV2& ir, const FillResult& fill, const ExportOpts& opts,
                  ExportResult& out, std::string& err) {
    if (!fill.mesh || !fill.anchors) {
        err = "delve/export: the fill result is incomplete (mesh or anchors missing)";
        return false;
    }
    if (opts.dir.empty() || opts.name.empty()) {
        err = "delve/export: dir and name are required";
        return false;
    }
    std::error_code ec;
    fs::create_directories(opts.dir, ec);
    if (ec) {
        err = "delve/export: cannot create " + opts.dir + ": " + ec.message();
        return false;
    }
    const fs::path dir(opts.dir);
    std::string pggErr;

    const std::string objPath = (dir / (opts.name + ".obj")).string();
    if (!pgg::writeObj(objPath, *fill.mesh, &pggErr)) {
        err = "delve/export [obj]: " + pggErr;
        return false;
    }
    out.written.push_back(objPath);

    const std::string anchorsPath = (dir / (opts.name + ".anchors.json")).string();
    if (!pgg::savePointsGeo(anchorsPath, *fill.anchors, &pggErr)) {
        err = "delve/export [anchors]: " + pggErr;
        return false;
    }
    out.written.push_back(anchorsPath);

    const std::string unitsPath = (dir / (opts.name + ".units.json")).string();
    if (!write_units_json(fill, unitsPath, err)) {
        err = "delve/export [units]: " + err;
        return false;
    }
    out.written.push_back(unitsPath);

    std::string irText;
    if (!write_ir_v2_json(ir, irText, err)) {
        err = "delve/export [ir]: " + err;
        return false;
    }
    const std::string irPath = (dir / (opts.name + ".ir.json")).string();
    if (!writeText(irPath, irText + '\n', err)) {
        err = "delve/export [ir]: " + err;
        return false;
    }
    out.written.push_back(irPath);

    if (opts.split_groups) {
        const pgg::GeoPtr grouped = meshWithUnitGroups(*fill.mesh, fill);
        std::vector<std::string> split;
        if (!pgg::writeObjSplitGroups(opts.dir, opts.name, *grouped, split, &pggErr)) {
            err = "delve/export [split]: " + pggErr;
            return false;
        }
        out.written.insert(out.written.end(), split.begin(), split.end());
    }
    return true;
}

}  // namespace delve
