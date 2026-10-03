#include "pch.h"

#include "filedialog.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <system_error>

#include <imgui.h>

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool isJson(const fs::path& p) { return lower(p.extension().string()) == ".json"; }

void syncPathBuf(FileDialog& st, const fs::path& p) {
    std::snprintf(st.pathBuf, sizeof(st.pathBuf), "%s", p.string().c_str());
}

void relist(FileDialog& st) {
    st.dirs.clear();
    st.files.clear();
    st.error.clear();
    std::error_code ec;
    if (st.dir.empty() || !fs::is_directory(st.dir, ec)) {
        st.error = "not a directory: " + st.dir.string();
        return;
    }
    for (fs::directory_iterator it(st.dir, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) break;
        const std::string name = it->path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        std::error_code ec2;
        if (it->is_directory(ec2)) {
            st.dirs.push_back(*it);
        } else if (it->is_regular_file(ec2) && isJson(it->path())) {
            st.files.push_back(*it);
        }
    }
    if (ec) st.error = ec.message();
    auto byName = [](const fs::directory_entry& a, const fs::directory_entry& b) {
        return lower(a.path().filename().string()) < lower(b.path().filename().string());
    };
    std::sort(st.dirs.begin(), st.dirs.end(), byName);
    std::sort(st.files.begin(), st.files.end(), byName);
    std::snprintf(st.dirBuf, sizeof(st.dirBuf), "%s", st.dir.string().c_str());
    st.dirty = false;
}

void gotoDir(FileDialog& st, const fs::path& dir) {
    std::error_code ec;
    const fs::path canon = fs::weakly_canonical(dir, ec);
    st.dir = ec ? dir : canon;
    st.selectedName.clear();
    syncPathBuf(st, st.dir);  // not a file: Select stays disabled until a pick
    st.dirty = true;
}

}  // namespace

void fileDialogOpen(FileDialog& st, const std::string& currentValue,
                    const std::string& fallbackDir) {
    std::error_code ec;
    fs::path dir;
    if (!currentValue.empty()) dir = fs::path(currentValue).parent_path();
    if (dir.empty() && !st.dir.empty()) dir = st.dir;  // session memory
    if (dir.empty()) dir = fs::path(fallbackDir);      // caller default (first open)
    if (dir.empty() || !fs::is_directory(dir, ec)) dir = fs::current_path(ec);
    gotoDir(st, dir);
    if (!currentValue.empty()) {
        const fs::path cur(currentValue);
        if (fs::is_regular_file(cur, ec)) {
            st.selectedName = cur.filename().string();
            syncPathBuf(st, cur);
        }
    }
    st.open = true;
}

bool fileDialogDraw(FileDialog& st, const char* title, std::string& outPath) {
    if (!st.open) return false;
    if (!ImGui::IsPopupOpen(title)) ImGui::OpenPopup(title);

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(640.0f, 460.0f), ImGuiCond_Appearing);

    std::string accepted;
    bool keepOpen = true;
    if (!ImGui::BeginPopupModal(title, &keepOpen, ImGuiWindowFlags_NoSavedSettings)) {
        st.open = false;
        return false;
    }
    if (st.dirty) relist(st);

    // Path row: Up + editable directory + Go.
    if (ImGui::Button("Up")) {
        const fs::path parent = st.dir.parent_path();
        if (!parent.empty() && parent != st.dir) gotoDir(st, parent);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-48.0f);
    const bool dirEntered =
        ImGui::InputText("##dir", st.dirBuf, sizeof(st.dirBuf), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Go") || dirEntered) {
        std::error_code ec;
        const fs::path typed(st.dirBuf);
        if (fs::is_regular_file(typed, ec)) {
            st.selectedName = typed.filename().string();
            syncPathBuf(st, typed);
        } else {
            gotoDir(st, typed);
        }
    }

    if (!st.error.empty()) ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.35f, 1.0f), "%s", st.error.c_str());

    // Listing: directories first (click enters), then .json files (click
    // selects, double-click accepts).
    const float footer = ImGui::GetFrameHeightWithSpacing() * 2.0f;
    ImGui::BeginChild("##listing", ImVec2(0.0f, -footer), true);
    for (const fs::directory_entry& d : st.dirs) {
        const std::string label = "[dir] " + d.path().filename().string();
        if (ImGui::Selectable(label.c_str())) {
            gotoDir(st, d.path());
            break;  // listing invalidated
        }
    }
    for (const fs::directory_entry& f : st.files) {
        const std::string name = f.path().filename().string();
        if (ImGui::Selectable(name.c_str(), name == st.selectedName,
                              ImGuiSelectableFlags_AllowDoubleClick)) {
            st.selectedName = name;
            syncPathBuf(st, f.path());
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) accepted = f.path().string();
        }
    }
    if (st.dirs.empty() && st.files.empty() && st.error.empty())
        ImGui::TextDisabled("(no .json files here)");
    ImGui::EndChild();

    // Editable selected path; Enter = Select.
    ImGui::SetNextItemWidth(-1.0f);
    const bool pathEntered = ImGui::InputText("##picked", st.pathBuf, sizeof(st.pathBuf),
                                              ImGuiInputTextFlags_EnterReturnsTrue);

    std::error_code ec;
    const bool valid = fs::is_regular_file(fs::path(st.pathBuf), ec);
    const float btnW = 90.0f;
    const float rowRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    if (!valid) ImGui::BeginDisabled();
    if (ImGui::Button("Select", ImVec2(btnW, 0.0f)) ||
        (valid && (pathEntered ||
                   (ImGui::IsKeyPressed(ImGuiKey_Enter, false) && !ImGui::GetIO().WantTextInput))))
        accepted = st.pathBuf;
    if (!valid) ImGui::EndDisabled();
    ImGui::SameLine(rowRight - btnW);
    if (ImGui::Button("Cancel", ImVec2(btnW, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        keepOpen = false;

    if (!accepted.empty() || !keepOpen) {
        st.open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    if (!accepted.empty()) {
        outPath = accepted;
        return true;
    }
    return false;
}
