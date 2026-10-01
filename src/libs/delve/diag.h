#pragma once

// Delve diagnostics (F10): machine-readable errors for the machine loop.
// Libraries still report flat `std::string& err`; the CLI layer wraps each
// step's err into a Diag carrying the step's class code. Codes are grouped by
// the F10 classes (registry: docs/cli_v1.md): D1xx project load, D2xx
// invariant (5.4/5.2), D3xx layout, D4xx slot (R-A3), D5xx PGG run, D6xx check
// (F11). R-A3/PGG codes (delve/slot, E100...) pass through inside messages.

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

// {"diagnostics":[{"code","message","hint"?,"warning"?}...]} — ordered keys (N6).
inline std::string diags_to_json(const std::vector<Diag>& diags) {
    nlohmann::ordered_json arr = nlohmann::ordered_json::array();
    for (const Diag& d : diags) {
        nlohmann::ordered_json j{{"code", d.code}, {"message", d.message}};
        if (!d.hint.empty()) j["hint"] = d.hint;
        if (d.warning) j["warning"] = true;
        arr.push_back(std::move(j));
    }
    return nlohmann::ordered_json{{"diagnostics", std::move(arr)}}.dump(2);
}

}  // namespace delve
