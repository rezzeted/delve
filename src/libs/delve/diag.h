#pragma once

// Delve diagnostics (F10): machine-readable errors for the machine loop.
// Libraries still report flat `std::string& err`; the CLI/serve layer wraps
// each step's err into a Diag carrying the step's class code. Codes are
// grouped by the F10 classes (registry: docs/cli_v1.md): D1xx project load,
// D2xx invariant (5.4/5.2), D3xx layout, D4xx slot (R-A3), D5xx PGG run,
// D6xx check (F11). R-A3/PGG codes (delve/slot, E100...) pass through inside
// messages.

#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace delve {

struct Diag {
    std::string code;     // "D101"; class codes per docs/cli_v1.md
    std::string message;  // place + expectation/fact, as the libraries phrase it
    std::string hint;     // fix suggestion, when known
    bool warning = false;
};

inline Diag make_diag(std::string code, std::string message, std::string hint = {}) {
    return Diag{std::move(code), std::move(message), std::move(hint), false};
}

// PGG-style text: "D301 <message>" + optional "\n  hint: <hint>".
inline std::string format_diag(const Diag& d) {
    std::string out = d.code + (d.warning ? " warning: " : " ") + d.message;
    if (!d.hint.empty()) out += "\n  hint: " + d.hint;
    return out;
}

inline nlohmann::ordered_json diag_to_json(const Diag& d) {
    nlohmann::ordered_json j{{"code", d.code}, {"message", d.message}};
    if (!d.hint.empty()) j["hint"] = d.hint;
    if (d.warning) j["warning"] = true;
    return j;
}

// [{"code","message","hint"?,"warning"?}...] — ordered keys (N6).
inline nlohmann::ordered_json diags_to_json(const std::vector<Diag>& diags) {
    nlohmann::ordered_json arr = nlohmann::ordered_json::array();
    for (const Diag& d : diags) arr.push_back(diag_to_json(d));
    return arr;
}

// load_project errors: 5.4 invariant -> D200, foreign format (N7) -> D102,
// io -> D100, anything else (unknown key, type, range) -> D101.
inline Diag classify_project_error(const std::string& err) {
    if (err.find("[5.4") != std::string::npos) return make_diag("D200", err);
    if (err.find("format") != std::string::npos &&
        (err.find("unsupported") != std::string::npos || err.find("expected") != std::string::npos))
        return make_diag("D102", err,
                         "re-save the file with the current Delve or migrate it to "
                         "delve-project/1");
    if (err.find("cannot") != std::string::npos &&
        (err.find("open") != std::string::npos || err.find("read") != std::string::npos))
        return make_diag("D100", err);
    return make_diag("D101", err);
}

// fill_level errors: delve/slot -> D4xx (R-A3), delve/run -> D5xx (PGG run);
// the message carries the slot/run codes verbatim.
inline Diag classify_fill_error(const std::string& err) {
    return make_diag(err.rfind("delve/slot", 0) == 0 ? "D400" : "D500", err);
}

}  // namespace delve
