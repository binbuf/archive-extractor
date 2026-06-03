#pragma once

#include <string>

namespace ae {

// Outcome of the orchestrated extraction flow driven behind the progress
// dialog. Maps to a process exit code by the caller (main.cpp).
enum class DialogOutcome {
    Success,    // extracted + placed; reveal hook fired (or empty archive)
    Cancelled,  // user cancelled (Cancel / Esc / X); temp removed, no reveal
    Failed,     // detection/extraction/placement error (error UI shown by caller)
};

// Result of RunExtractionDialog.
struct DialogResult {
    DialogOutcome outcome = DialogOutcome::Failed;
    // On failure, a short user-facing message describing the problem (already
    // includes the archive name). Empty otherwise. The caller routes this to the
    // placeholder error UI (real error dialog is task 06).
    std::wstring errorMessage;
};

// Run the full extraction flow behind a small, centered, modeless progress
// dialog:
//   launch -> detect (02) -> stage+extract (03) -> place (04) -> reveal (07 stub)
//
// ALL extraction work runs on a worker thread; this function pumps the UI
// message loop on the calling (UI) thread and returns only once the flow
// finishes (success, cancel, or error) and the window has closed.
//
//  - archivePath : absolute path to the archive (already validated to exist and
//                  have a supported extension by the caller).
//  - hInstance   : the module instance (for window-class registration).
//
// The window class is registered lazily on first call. This is intended to be
// invoked once per process (the app is single-shot), but is safe to call again.
DialogResult RunExtractionDialog(const std::wstring& archivePath,
                                 void* hInstance);

}  // namespace ae
