#pragma once

// Minimal ImGui file dialog for picking .json files (no OS dialog dependency:
// the viewer runs on sokol_app across win/mac/linux and native dialog
// libraries are not pulled in). Directory listing via std::filesystem.
// Reusable: the caller owns one state per target field — the instance
// remembers its directory for the session.

#include <filesystem>
#include <string>
#include <vector>

struct FileDialog {
    bool open = false;            // popup requested / visible
    std::filesystem::path dir;    // directory being browsed
    std::string selectedName;     // highlighted file (name only)
    std::string error;            // last listing error, shown inline
    std::vector<std::filesystem::directory_entry> dirs;   // subdirs, by name
    std::vector<std::filesystem::directory_entry> files;  // *.json, by name
    bool dirty = true;            // relist on next draw
    char dirBuf[1024] = {};       // editable current directory
    char pathBuf[1024] = {};      // editable selected full path (Enter = Select)
};

// Opens the dialog: at the parent of currentValue when set, else at the
// session-remembered directory of this instance, else at fallbackDir when it
// is a directory, else at the cwd.
void fileDialogOpen(FileDialog& st, const std::string& currentValue,
                    const std::string& fallbackDir = {});

// Draws the modal while open. On Select / double-click / Enter writes the
// chosen path to outPath and returns true (once). Cancel / Esc just closes.
bool fileDialogDraw(FileDialog& st, const char* title, std::string& outPath);
