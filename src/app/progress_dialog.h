#pragma once

#include <string>

#include "archive_core/error_model.h"

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
    // On failure, the unified error model entry (code + plain-language message
    // naming the archive). The caller (main.cpp) routes `error` to the real
    // error dialog (task 06). `error.message` mirrors the displayed body; on
    // success/cancel `error.code` is ErrorCode::None.
    ErrorInfo error;
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
