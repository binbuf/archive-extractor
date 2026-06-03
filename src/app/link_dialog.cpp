#include "link_dialog.h"

#include <format>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>  // TaskDialogIndirect

namespace ae {

namespace {
constexpr int kBtnCopy = 101;
constexpr int kBtnSkip = 102;
}  // namespace

LinkPolicy ShowLinkDialog(const std::wstring& archiveName, void* owner,
                          void* hInstance) {
    const std::wstring instruction = L"This archive contains links.";
    const std::wstring content = std::format(
        L"“{}” contains symbolic or hard links. Copy the files they point to, "
        L"or skip the links?\n\nLinks to files outside the archive can't be "
        L"copied and are skipped.",
        archiveName);

    const TASKDIALOG_BUTTON buttons[] = {
        {kBtnCopy, L"Copy targets\nReplace each link with a copy of its target."},
        {kBtnSkip, L"Skip links\nLeave the links out of the extracted files."},
    };

    TASKDIALOGCONFIG cfg{};
    cfg.cbSize = sizeof(cfg);
    cfg.hwndParent = static_cast<HWND>(owner);
    cfg.hInstance = static_cast<HINSTANCE>(hInstance);
    cfg.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION |
                  TDF_POSITION_RELATIVE_TO_WINDOW;
    cfg.dwCommonButtons = 0;
    cfg.pszWindowTitle = L"Archive Extractor";
    cfg.pszMainIcon = TD_INFORMATION_ICON;
    cfg.pszMainInstruction = instruction.c_str();
    cfg.pszContent = content.c_str();
    cfg.pButtons = buttons;
    cfg.cButtons = ARRAYSIZE(buttons);
    cfg.nDefaultButton = kBtnSkip;  // safe default if the user just presses Enter

    int pressed = 0;
    const HRESULT hr = TaskDialogIndirect(&cfg, &pressed, nullptr, nullptr);
    if (SUCCEEDED(hr) && pressed == kBtnCopy) return LinkPolicy::Copy;
    // Skip button, Esc, close, or any failure -> the safe default.
    return LinkPolicy::Skip;
}

}  // namespace ae
