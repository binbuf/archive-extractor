#pragma once

#include <string>

namespace ae {

// Result of the modal password prompt.
struct PasswordPromptResult {
    bool cancelled = true;     // true if the user pressed Cancel / Esc / closed
    std::wstring password;     // the entered password (valid only if !cancelled)
};

// Show the modal password prompt (design 05 §Password prompt). The dialog names
// the archive ("\"secret.7z\" is password protected."), masks the input, focuses
// the edit on open, treats Enter as OK and Esc as Cancel, and is centered,
// DPI-aware, and themed via the common-controls manifest.
//
//  - archiveName : archive leaf name (with extension) shown in the prompt.
//  - attempt     : 0 for the first prompt; >= 1 shows the inline
//                  "Incorrect password — try again." message above the field.
//  - owner       : owning HWND (cast to void*) disabled while the dialog is up;
//                  may be nullptr for a top-level prompt.
//  - hInstance   : module instance for the window class (cast to void*).
//
// The returned password lives only in the result string; the dialog zeroes its
// own internal buffers before returning. The password is never logged.
PasswordPromptResult ShowPasswordDialog(const std::wstring& archiveName,
                                        unsigned attempt, void* owner,
                                        void* hInstance);

}  // namespace ae
