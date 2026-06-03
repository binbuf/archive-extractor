#pragma once

#include <string>

#include "archive_core/extract.h"  // ae::LinkPolicy

namespace ae {

// Show the one-time "this archive contains links" prompt (design 07 §6) and
// return the chosen policy. Themed TaskDialog with two buttons — "Copy targets"
// and "Skip links". Dismissing the dialog (Esc / close) defaults to Skip, the
// safe no-privilege choice. Shown at most once per extraction by the engine.
//
//  - archiveName : archive leaf name (with extension), named in the prompt.
//  - owner       : owning HWND (cast to void*); may be nullptr.
//  - hInstance   : module instance (cast to void*).
LinkPolicy ShowLinkDialog(const std::wstring& archiveName, void* owner,
                          void* hInstance);

}  // namespace ae
