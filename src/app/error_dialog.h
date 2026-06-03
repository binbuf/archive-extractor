#pragma once

#include "archive_core/error_model.h"

namespace ae {

// Show the single, modal error dialog for a failed extraction (task 06). Title
// "Archive Extractor", a plain-language body naming the archive (info.message),
// and a single OK that dismisses it. Uses a themed TaskDialogIndirect when
// available, falling back to MessageBox. The caller exits the process after
// this returns (no partial output has been left behind by the flow).
//
// `owner` is an optional owner HWND (cast to void* to keep this header
// Win32-free for callers that don't include windows.h); pass nullptr for a
// top-level dialog.
void ShowErrorDialog(const ErrorInfo& info, void* owner = nullptr);

}  // namespace ae
