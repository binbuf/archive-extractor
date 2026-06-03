#include "error_dialog.h"

#include "archive_core/logging.h"

#include <format>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>  // TaskDialogIndirect

namespace ae {

namespace {

constexpr const wchar_t* kTitle = L"Archive Extractor";

// Themed task dialog (comctl32 v6 via the app manifest). Returns false if the
// API is unavailable (very old OS) so the caller can fall back to MessageBox.
bool ShowTaskDialog(const std::wstring& body, HWND owner) {
    TASKDIALOGCONFIG cfg{};
    cfg.cbSize = sizeof(cfg);
    cfg.hwndParent = owner;
    cfg.dwCommonButtons = TDCBF_OK_BUTTON;
    cfg.pszWindowTitle = kTitle;
    cfg.pszMainIcon = TD_ERROR_ICON;
    // The whole plain-language sentence reads naturally as the main instruction.
    cfg.pszMainInstruction = body.c_str();
    cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;

    int pressed = 0;
    const HRESULT hr = TaskDialogIndirect(&cfg, &pressed, nullptr, nullptr);
    return SUCCEEDED(hr);
}

}  // namespace

void ShowErrorDialog(const ErrorInfo& info, void* owner) {
    HWND hwndOwner = static_cast<HWND>(owner);
    const std::wstring body =
        info.message.empty() ? std::wstring(L"Couldn't expand the archive.")
                             : info.message;

    // Diagnostic breadcrumb (code only — never contents/passwords).
    Log(std::format(L"[error-dialog] code={} archive={}",
                    ErrorCodeToken(info.code), info.archiveName));

    if (ShowTaskDialog(body, hwndOwner)) return;

    // Fallback for environments without the themed task dialog.
    MessageBoxW(hwndOwner, body.c_str(), kTitle, MB_OK | MB_ICONERROR);
}

}  // namespace ae
