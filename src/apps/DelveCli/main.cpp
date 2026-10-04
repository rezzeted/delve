// DelveCli (D4, F10): the machine loop — every pipeline step as a CLI command
// with PGG-style diagnostics (code, place, expectation/fact, hint), grouped by
// the F10 classes (registry: docs/cli_v1.md): D1xx project load, D2xx
// invariant (5.4/5.2), D3xx layout, D4xx slot, D5xx PGG run, D6xx check.
// Exit codes (repo convention): 0 ok, 1 diagnostics with errors, 2 usage/io.
// Libraries report flat strings; this layer wraps them into delve::Diag.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "assets.h"
#include "catalog.h"
#include "check.h"
#include "diag.h"
#include "export.h"
#include "fill.h"
#include "generate.h"
#include "ir.h"
#include "layout.h"
#include "project.h"

namespace {

namespace fs = std::filesystem;

constexpr int kLayoutAttempts = 4;

double nowMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool readTextFile(const std::string& path, std::string& text) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    text = ss.str();
    return true;
}

// --- args -------------------------------------------------------------------

struct CliArgs {
    std::string command;
    std::string project;
    std::string irFile;      // --ir: delve-ir/0|2|3, sniffed by the format key
    std::string layoutFile;  // --layout: delve-layout/0
    std::string outFile;     // -o
    std::string name;        // --name (export artifact base name)
    std::string assets;      // --assets
    std::string unit;        // --unit (check: only units whose id has this substring)
    std::optional<int> seed;
    int attempts = kLayoutAttempts;
    unsigned threads = 0;
    bool json = false;
    bool splitGroups = false;
    bool help = false;
};

void printUsage(std::ostream& out) {
    out <<
        "DelveCli — the delve machine loop (F10): project -> export via CLI\n"
        "\n"
        "usage: DelveCli <command> [args] [--json] [--assets <dir>]\n"
        "\n"
        "commands:\n"
        "  validate <project.json>                                 F1: load + check the project\n"
        "  layout   <project.json> [--seed N] [--attempts N] [-o layout.json]\n"
        "                                                          F2/F3: catalog + edgar layout\n"
        "  ir       <project.json> [--layout f | --ir f] [-o ir.json]\n"
        "                                                          F4/F5: build or normalize the IR\n"
        "  fill     <project.json> [--ir f | --layout f] [--threads N]\n"
        "                                                          F6: fill the level (stats only)\n"
        "  export   <project.json> [--ir f | --layout f] [-o dir] [--name n]\n"
        "           [--split-groups]                               F6+F7: obj + anchors + units + ir\n"
        "  check    <project.json> [--ir f | --layout f] [--threads N] [--unit s]\n"
        "                                                          F6+F11: geometric checks\n"
        "                                                          --unit s: only units with 's' in the id\n"
        "\n"
        "Without --ir/--layout the IR is built from the project's layout tier\n"
        "(delve-project/1). --ir files are sniffed by the format key: delve-ir/0\n"
        "(frozen, built through the project) or delve-ir/2|3 (read directly).\n"
        "Without -o the layout/ir commands print the artifact on stdout.\n"
        "\n"
        "exit codes: 0 ok, 1 diagnostics with errors, 2 usage/io error.\n"
        "--json prints one {\"command\",\"ok\",\"diagnostics\",...} object on stdout.\n";
}

bool parseArgs(int argc, char** argv, CliArgs& args, std::string& err) {
    auto needValue = [&](int& i, const std::string& flag, std::string& dst) -> bool {
        if (i + 1 >= argc) {
            err = flag + " expects a value";
            return false;
        }
        dst = argv[++i];
        return true;
    };
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            args.help = true;
        } else if (a == "--json") {
            args.json = true;
        } else if (a == "--split-groups") {
            args.splitGroups = true;
        } else if (a == "--ir") {
            if (!needValue(i, a, args.irFile)) return false;
        } else if (a == "--layout") {
            if (!needValue(i, a, args.layoutFile)) return false;
        } else if (a == "-o" || a == "--out") {
            if (!needValue(i, a, args.outFile)) return false;
        } else if (a == "--name") {
            if (!needValue(i, a, args.name)) return false;
        } else if (a == "--assets") {
            if (!needValue(i, a, args.assets)) return false;
        } else if (a == "--unit") {
            if (!needValue(i, a, args.unit)) return false;
        } else if (a == "--seed" || a == "--attempts" || a == "--threads") {
            std::string v;
            if (!needValue(i, a, v)) return false;
            int n = 0;
            try {
                n = std::stoi(v);
            } catch (...) {
                err = a + " expects an integer, got \"" + v + "\"";
                return false;
            }
            if (a == "--seed") {
                args.seed = n;
            } else if (a == "--attempts") {
                if (n < 1) {
                    err = "--attempts must be >= 1";
                    return false;
                }
                args.attempts = n;
            } else {
                if (n < 0) {
                    err = "--threads must be >= 0";
                    return false;
                }
                args.threads = static_cast<unsigned>(n);
            }
        } else if (!a.empty() && a[0] == '-') {
            err = "unknown flag \"" + a + "\"";
            return false;
        } else if (args.command.empty()) {
            args.command = a;
        } else if (args.project.empty()) {
            args.project = a;
        } else {
            err = "unexpected argument \"" + a + "\"";
            return false;
        }
    }
    if (args.help) return true;
    if (args.command.empty()) {
        err = "no command";
        return false;
    }
    static const std::vector<std::string> kCommands = {"validate", "layout", "ir",
                                                       "fill",     "export", "check"};
    bool known = false;
    for (const std::string& c : kCommands)
        if (args.command == c) known = true;
    if (!known) {
        err = "unknown command \"" + args.command + "\"";
        return false;
    }
    if (args.project.empty()) {
        err = args.command + " expects <project.json>";
        return false;
    }
    return true;
}

// --- pipeline context -------------------------------------------------------

struct Context {
    std::string projectPath;
    delve::Project project;
    bool hasProject = false;
    delve::LayoutData layoutData;
    std::string layoutText;  // canonical delve-layout/0 handoff text
    bool hasLayout = false;
    delve::IrV2 ir;
    bool hasIr = false;
    delve::FillResult fill;
    bool hasFill = false;
    delve::UnitCache cache;  // F8: lives between the steps of one command
    double layoutMs = 0;
    double fillMs = 0;
    int seedUsed = 0;
    int attemptUsed = 0;
};

bool ensureProject(Context& ctx, const CliArgs& args, std::vector<delve::Diag>& diags) {
    if (ctx.hasProject) return true;
    std::string err;
    if (!delve::load_project(ctx.projectPath, ctx.project, err)) {
        diags.push_back(delve::classify_project_error(err));
        return false;
    }
    if (args.seed) ctx.project.seed = *args.seed;
    ctx.hasProject = true;
    return true;
}

// Catalog + edgar + the delve-layout/0 handoff (serialize, parse back) — or a
// --layout file read directly.
bool ensureLayout(Context& ctx, const CliArgs& args, std::vector<delve::Diag>& diags) {
    if (ctx.hasLayout) return true;
    std::string err;
    if (!args.layoutFile.empty()) {
        if (!readTextFile(args.layoutFile, ctx.layoutText)) {
            diags.push_back(delve::make_diag("D100", "cannot open " + args.layoutFile));
            return false;
        }
        if (!delve::read_layout_json(ctx.layoutText, ctx.layoutData, err)) {
            diags.push_back(delve::make_diag("D300", err));
            return false;
        }
        ctx.hasLayout = true;
        return true;
    }
    if (!ctx.project.layout) {
        diags.push_back(delve::make_diag(
            "D101", ctx.projectPath + ": delve-project/0 has no layout tier; pass --ir or --layout",
            "use a delve-project/1 project or feed a saved IR via --ir"));
        return false;
    }
    delve::layout::Catalog catalog;
    if (!delve::layout::build_catalog(ctx.project, catalog, err)) {
        diags.push_back(delve::make_diag("D300", err));
        return false;
    }
    delve::layout::LayoutGenerator gen;
    delve::layout::GenerateOptions opts;
    opts.attempts = args.attempts;
    delve::layout::LayoutResult result;
    const double t0 = nowMs();
    if (!gen.generate(ctx.project, catalog, opts, result, err)) {
        diags.push_back(delve::make_diag("D301", err,
                                         "raise --attempts or the layout budget, or loosen the "
                                         "graph/catalog"));
        return false;
    }
    ctx.layoutMs = nowMs() - t0;
    ctx.seedUsed = result.seed_used;
    ctx.attemptUsed = result.attempt_used;
    if (!delve::layout::write_layout_json(result, ctx.project, ctx.projectPath, ctx.layoutText,
                                          err)) {
        diags.push_back(delve::make_diag("D300", err));
        return false;
    }
    if (!delve::read_layout_json(ctx.layoutText, ctx.layoutData, err)) {
        diags.push_back(delve::make_diag("D300", err));
        return false;
    }
    ctx.hasLayout = true;
    return true;
}

bool ensureIr(Context& ctx, const CliArgs& args, std::vector<delve::Diag>& diags) {
    if (ctx.hasIr) return true;
    std::string err;
    if (!args.irFile.empty()) {
        std::string text;
        if (!readTextFile(args.irFile, text)) {
            diags.push_back(delve::make_diag("D100", "cannot open " + args.irFile));
            return false;
        }
        // delve-ir/0 (frozen) builds through the project; delve-ir/2|3 reads
        // directly. The format key decides (N7: reject the rest).
        const nlohmann::json doc = nlohmann::json::parse(text, nullptr, false);
        const std::string format =
            doc.is_object() ? doc.value("format", std::string{}) : std::string{};
        if (format == "delve-ir/0") {
            if (!delve::build_ir_v2(text, args.irFile, ctx.project, ctx.projectPath, ctx.ir, err)) {
                diags.push_back(delve::make_diag("D200", err));
                return false;
            }
        } else if (format == delve::kIrFormat || format == delve::kIrFormatV2) {
            if (!delve::read_ir_v2_json(text, ctx.ir, err)) {
                diags.push_back(delve::make_diag("D102", err));
                return false;
            }
        } else {
            diags.push_back(delve::make_diag(
                "D102",
                args.irFile + ": unsupported IR format \"" + format + "\" (expected delve-ir/0, " +
                    delve::kIrFormatV2 + " or " + delve::kIrFormat + ")",
                "re-export the IR with the current DelveCli or migrate it to " +
                    std::string(delve::kIrFormat)));
            return false;
        }
        ctx.hasIr = true;
        return true;
    }
    if (!ensureLayout(ctx, args, diags)) return false;
    if (!delve::build_ir_from_layout(ctx.layoutData, ctx.project, ctx.projectPath, ctx.ir, err)) {
        diags.push_back(delve::make_diag("D200", err));
        return false;
    }
    ctx.hasIr = true;
    return true;
}

bool ensureFill(Context& ctx, const CliArgs& args, const std::string& argv0,
                std::vector<delve::Diag>& diags) {
    if (ctx.hasFill) return true;
    const std::string assets =
        !args.assets.empty() ? args.assets : delve::find_delve_assets(argv0, ctx.projectPath);
    if (assets.empty()) {
        diags.push_back(delve::make_diag(
            "D100",
            "cannot locate the delve assets dir (no assets/codes.pgg from the cwd, the executable "
            "or the project dir upwards)",
            "pass --assets <dir>"));
        return false;
    }
    delve::FillOpts opts;
    opts.delve_assets = assets;
    opts.threads = args.threads;
    opts.cache = &ctx.cache;
    std::string err;
    const double t0 = nowMs();
    if (!delve::fill_level(ctx.ir, ctx.project, opts, ctx.fill, err)) {
        diags.push_back(delve::classify_fill_error(err));
        return false;
    }
    ctx.fillMs = nowMs() - t0;
    ctx.hasFill = true;
    return true;
}

// --- reporting ----------------------------------------------------------------

// Ends a command: text mode prints stats on stdout and diags on stderr; --json
// prints a single envelope on stdout. Returns the exit code.
int finishCmd(const CliArgs& args, int code, const std::vector<delve::Diag>& diags,
              const nlohmann::ordered_json& stats = nlohmann::ordered_json(),
              const std::string& statsText = {}) {
    if (args.json) {
        nlohmann::ordered_json env{{"command", args.command},
                                   {"ok", code == 0},
                                   {"diagnostics", delve::diags_to_json(diags)}};
        if (!stats.is_null() && !stats.empty()) env["stats"] = stats;
        std::cout << env.dump(2) << '\n';
    } else {
        if (code == 0 && !statsText.empty()) std::cout << statsText << '\n';
        for (const delve::Diag& d : diags) std::cerr << delve::format_diag(d) << '\n';
    }
    return code;
}

bool writeOut(const std::string& path, const std::string& text,
              std::vector<delve::Diag>& diags) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out || !(out << text)) {
        diags.push_back(delve::make_diag("D100", "cannot write " + path));
        return false;
    }
    return true;
}

// Export artifact base name: the project stem; a bare "project.json" names its
// directory instead (projects/demo/project.json -> "demo").
std::string defaultExportName(const std::string& projectPath) {
    const fs::path p(projectPath);
    const std::string stem = p.stem().string();
    if (stem != "project") return stem;
    const std::string parent = p.parent_path().filename().string();
    return parent.empty() ? stem : parent;
}

nlohmann::ordered_json fillStatsJson(const Context& ctx) {
    const delve::FillStats& s = ctx.fill.stats;
    return {{"rooms", s.rooms},   {"bodies", s.bodies},       {"facings", s.facings},
            {"nodes", s.nodes},   {"doors", s.doors},         {"lamps", s.lamps},
            {"reused", s.reused.size()}, {"reran", s.reran.size()},
            {"ms", static_cast<long long>(ctx.fillMs)}};
}

std::string fillStatsText(const Context& ctx) {
    const delve::FillStats& s = ctx.fill.stats;
    std::ostringstream out;
    out << "fill: " << s.rooms << " rooms, " << s.bodies << " bodies, " << s.facings
        << " facings, " << s.nodes << " nodes, " << s.doors << " doors, " << s.lamps
        << " lamps; cache reused " << s.reused.size() << ", reran " << s.reran.size() << "; "
        << static_cast<long long>(ctx.fillMs) << " ms";
    return out.str();
}

// --- commands -----------------------------------------------------------------

int cmdValidate(Context& ctx, const CliArgs& args) {
    std::vector<delve::Diag> diags;
    if (!ensureProject(ctx, args, diags)) return finishCmd(args, 1, diags);
    return finishCmd(args, 0, diags, {{"seed", ctx.project.seed}},
                     "validate: ok (" + ctx.projectPath + ")");
}

int cmdLayout(Context& ctx, const CliArgs& args) {
    std::vector<delve::Diag> diags;
    if (!ensureProject(ctx, args, diags) || !ensureLayout(ctx, args, diags))
        return finishCmd(args, 1, diags);
    nlohmann::ordered_json stats{{"seed_used", ctx.seedUsed},
                                 {"attempt_used", ctx.attemptUsed},
                                 {"ms", static_cast<long long>(ctx.layoutMs)}};
    std::ostringstream line;
    line << "layout: seed " << ctx.seedUsed << ", attempt " << ctx.attemptUsed << ", "
         << static_cast<long long>(ctx.layoutMs) << " ms";
    if (!args.outFile.empty()) {
        if (!writeOut(args.outFile, ctx.layoutText, diags)) return finishCmd(args, 2, diags);
        stats["wrote"] = args.outFile;
        line << "; wrote " << args.outFile;
        return finishCmd(args, 0, diags, stats, line.str());
    }
    if (args.json) return finishCmd(args, 0, diags, stats);
    std::cout << ctx.layoutText;  // the artifact, clean for piping
    std::cerr << line.str() << '\n';
    return 0;
}

int cmdIr(Context& ctx, const CliArgs& args) {
    std::vector<delve::Diag> diags;
    if (!ensureProject(ctx, args, diags) || !ensureIr(ctx, args, diags))
        return finishCmd(args, 1, diags);
    std::string err, text;
    if (!delve::write_ir_v2_json(ctx.ir, text, err)) {
        diags.push_back(delve::make_diag("D500", err));
        return finishCmd(args, 1, diags);
    }
    text += '\n';
    nlohmann::ordered_json stats{{"format", delve::kIrFormat}};
    if (!args.outFile.empty()) {
        if (!writeOut(args.outFile, text, diags)) return finishCmd(args, 2, diags);
        stats["wrote"] = args.outFile;
        return finishCmd(args, 0, diags, stats, "ir: wrote " + args.outFile);
    }
    if (args.json) return finishCmd(args, 0, diags, stats);
    std::cout << text;
    return 0;
}

int cmdFill(Context& ctx, const CliArgs& args, const std::string& argv0) {
    std::vector<delve::Diag> diags;
    if (!ensureProject(ctx, args, diags) || !ensureIr(ctx, args, diags) ||
        !ensureFill(ctx, args, argv0, diags))
        return finishCmd(args, 1, diags);
    return finishCmd(args, 0, diags, fillStatsJson(ctx), fillStatsText(ctx));
}

int cmdExport(Context& ctx, const CliArgs& args, const std::string& argv0) {
    std::vector<delve::Diag> diags;
    if (!ensureProject(ctx, args, diags) || !ensureIr(ctx, args, diags) ||
        !ensureFill(ctx, args, argv0, diags))
        return finishCmd(args, 1, diags);
    delve::ExportOpts opts;
    opts.dir = args.outFile.empty() ? "." : args.outFile;
    opts.name = args.name.empty() ? defaultExportName(ctx.projectPath) : args.name;
    opts.split_groups = args.splitGroups;
    delve::ExportResult res;
    std::string err;
    if (!delve::export_level(ctx.ir, ctx.fill, opts, res, err)) {
        // Export failures are io-class: plain error, exit 2.
        std::cerr << "error: " << err << '\n';
        return 2;
    }
    nlohmann::ordered_json stats = fillStatsJson(ctx);
    stats["written"] = res.written;
    std::ostringstream line;
    line << "export: wrote " << res.written.size() << " artifact(s) to " << opts.dir << " ("
         << static_cast<long long>(ctx.fillMs) << " ms fill)";
    if (!args.json) {
        std::cout << line.str() << '\n';
        for (const std::string& w : res.written) std::cout << "  " << w << '\n';
        return 0;
    }
    return finishCmd(args, 0, diags, stats, line.str());
}

int cmdCheck(Context& ctx, const CliArgs& args, const std::string& argv0) {
    std::vector<delve::Diag> diags;
    if (!ensureProject(ctx, args, diags) || !ensureIr(ctx, args, diags) ||
        !ensureFill(ctx, args, argv0, diags))
        return finishCmd(args, 1, diags);
    std::vector<delve::CheckDiag> checks;
    bool ok;
    nlohmann::ordered_json stats;
    if (args.unit.empty()) {
        ok = delve::check_level(ctx.ir, ctx.project, ctx.fill, checks);
        stats["errors"] = checks.size();
    } else {
        size_t matched = 0;
        ok = delve::check_units(ctx.fill, args.unit, checks, &matched);
        stats["errors"] = checks.size();
        stats["units"] = matched;
    }
    for (const delve::CheckDiag& c : checks)
        diags.push_back(delve::make_diag("D600", c.message));
    if (ok) return finishCmd(args, 0, diags, stats, "check: ok");
    return finishCmd(args, 1, diags, stats);
}

}  // namespace

int main(int argc, char** argv) {
    CliArgs args;
    std::string parseErr;
    if (!parseArgs(argc, argv, args, parseErr)) {
        if (!parseErr.empty()) std::cerr << "error: " << parseErr << '\n';
        printUsage(std::cerr);
        return 2;
    }
    if (args.help) {
        printUsage(std::cout);
        return 0;
    }
    Context ctx;
    ctx.projectPath = args.project;
    const std::string argv0 = argc > 0 ? argv[0] : "";
    if (args.command == "validate") return cmdValidate(ctx, args);
    if (args.command == "layout") return cmdLayout(ctx, args);
    if (args.command == "ir") return cmdIr(ctx, args);
    if (args.command == "fill") return cmdFill(ctx, args, argv0);
    if (args.command == "export") return cmdExport(ctx, args, argv0);
    if (args.command == "check") return cmdCheck(ctx, args, argv0);
    return 2;  // parseArgs rejects unknown commands; unreachable
}
