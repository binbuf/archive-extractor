#include "password_dialog.h"

#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace ae {

namespace {

const wchar_t* kClassName = L"ae.PasswordPromptDialog";
const wchar_t* kTitle = L"Archive Extractor";

// Control ids (IDOK/IDCANCEL are the standard 1/2 so IsDialogMessageW maps the
// default button + Esc correctly).
constexpr int kIdEdit = 100;

// Logical (DPI-unscaled) metrics; scaled per-monitor at create time.
constexpr int kDlgWidthDip = 360;
constexpr int kMarginDip = 16;
constexpr int kLineHeightDip = 18;
constexpr int kEditHeightDip = 24;
constexpr int kBtnWidthDip = 84;
constexpr int kBtnHeightDip = 26;
constexpr int kGapDip = 10;

struct PwState {
    std::wstring archiveName;
    unsigned attempt = 0;

    HWND hwnd = nullptr;
    HWND hTitle = nullptr;
    HWND hError = nullptr;   // inline "Incorrect password" (only when attempt>=1)
    HWND hLabel = nullptr;   // "Password:"
    HWND hEdit = nullptr;
    HWND hOk = nullptr;
    HWND hCancel = nullptr;
    HFONT hFont = nullptr;

    UINT dpi = 96;
    bool done = false;
    PasswordPromptResult result;  // filled on close
};

int Scale(int dip, UINT dpi) { return MulDiv(dip, static_cast<int>(dpi), 96); }

RECT ActiveMonitorRect() {
    POINT pt;
    HMONITOR mon = nullptr;
    if (GetCursorPos(&pt)) {
        mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    } else {
        mon = MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
    }
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi)) return mi.rcWork;
    RECT r{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    return r;
}

HFONT MakeUiFont(UINT dpi) {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0,
                                   dpi)) {
        return CreateFontIndirectW(&ncm.lfMessageFont);
    }
    return static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

// The content height depends on whether the inline error line is shown.
int ContentHeightDip(bool withError) {
    int h = kMarginDip;
    h += kLineHeightDip + kGapDip;       // title
    if (withError) h += kLineHeightDip + kGapDip;  // inline error
    h += kEditHeightDip + kGapDip;       // password row
    h += kBtnHeightDip + kMarginDip;     // buttons
    return h;
}

void LayoutChildren(PwState* st) {
    const UINT dpi = st->dpi;
    const int margin = Scale(kMarginDip, dpi);
    const int gap = Scale(kGapDip, dpi);
    const int lineH = Scale(kLineHeightDip, dpi);
    const int editH = Scale(kEditHeightDip, dpi);
    const int btnW = Scale(kBtnWidthDip, dpi);
    const int btnH = Scale(kBtnHeightDip, dpi);
    const bool withError = st->hError != nullptr;

    RECT rc;
    GetClientRect(st->hwnd, &rc);
    const int cw = rc.right - rc.left;
    const int contentW = cw - 2 * margin;

    int y = margin;
    SetWindowPos(st->hTitle, nullptr, margin, y, contentW, lineH,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    y += lineH + gap;

    if (withError) {
        SetWindowPos(st->hError, nullptr, margin, y, contentW, lineH,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        y += lineH + gap;
    }

    // "Password:" label sits left of the edit on the same row.
    const int labelW = Scale(70, dpi);
    SetWindowPos(st->hLabel, nullptr, margin, y + (editH - lineH) / 2, labelW,
                 lineH, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(st->hEdit, nullptr, margin + labelW, y, contentW - labelW,
                 editH, SWP_NOZORDER | SWP_NOACTIVATE);
    y += editH + gap;

    // OK + Cancel right-aligned.
    const int cancelX = margin + contentW - btnW;
    const int okX = cancelX - gap - btnW;
    SetWindowPos(st->hOk, nullptr, okX, y, btnW, btnH,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(st->hCancel, nullptr, cancelX, y, btnW, btnH,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void CenterAndSize(PwState* st) {
    const UINT dpi = st->dpi;
    const int w = Scale(kDlgWidthDip, dpi);
    const int clientH = Scale(ContentHeightDip(st->hError != nullptr), dpi);
    RECT nc{0, 0, 0, clientH};
    AdjustWindowRectExForDpi(&nc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE,
                             WS_EX_DLGMODALFRAME, dpi);
    const int h = nc.bottom - nc.top;

    const RECT work = ActiveMonitorRect();
    const int x = work.left + ((work.right - work.left) - w) / 2;
    const int y = work.top + ((work.bottom - work.top) - h) / 2;
    SetWindowPos(st->hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

void ApplyFont(PwState* st) {
    if (st->hFont) DeleteObject(st->hFont);
    st->hFont = MakeUiFont(st->dpi);
    auto setFont = [&](HWND h) {
        if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(st->hFont), TRUE);
    };
    setFont(st->hTitle);
    setFont(st->hError);
    setFont(st->hLabel);
    setFont(st->hEdit);
    setFont(st->hOk);
    setFont(st->hCancel);
}

// Read the edit text into `out`, then zero the transient buffer.
void CapturePassword(PwState* st, std::wstring& out) {
    const int len = GetWindowTextLengthW(st->hEdit);
    if (len <= 0) {
        out.clear();
        return;
    }
    std::wstring buf(static_cast<std::size_t>(len) + 1, L'\0');
    const int got = GetWindowTextW(st->hEdit, buf.data(),
                                   static_cast<int>(buf.size()));
    buf.resize(static_cast<std::size_t>(got < 0 ? 0 : got));
    out = buf;
    if (!buf.empty()) SecureZeroMemory(buf.data(), buf.size() * sizeof(wchar_t));
}

void Finish(PwState* st, bool cancelled) {
    if (st->done) return;
    st->result.cancelled = cancelled;
    if (!cancelled) {
        CapturePassword(st, st->result.password);
    }
    // Wipe whatever the edit control still holds.
    SetWindowTextW(st->hEdit, L"");
    st->done = true;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    PwState* st = reinterpret_cast<PwState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            st = reinterpret_cast<PwState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(st));
            st->hwnd = hwnd;
            st->dpi = GetDpiForWindow(hwnd);
            HINSTANCE inst = cs->hInstance;

            const std::wstring titleText =
                L"“" + st->archiveName + L"” is password protected.";
            st->hTitle = CreateWindowExW(
                0, L"STATIC", titleText.c_str(),
                WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX,
                0, 0, 0, 0, hwnd, nullptr, inst, nullptr);

            if (st->attempt >= 1) {
                st->hError = CreateWindowExW(
                    0, L"STATIC", L"Incorrect password — try again.",
                    WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 0, 0,
                    hwnd, nullptr, inst, nullptr);
            }

            st->hLabel = CreateWindowExW(
                0, L"STATIC", L"Password:",
                WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 0, 0, hwnd,
                nullptr, inst, nullptr);
            st->hEdit = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL,
                0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdEdit)), inst,
                nullptr);
            st->hOk = CreateWindowExW(
                0, L"BUTTON", L"OK",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 0, 0,
                hwnd, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDOK)), inst,
                nullptr);
            st->hCancel = CreateWindowExW(
                0, L"BUTTON", L"Cancel",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0,
                hwnd,
                reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDCANCEL)), inst,
                nullptr);

            ApplyFont(st);
            CenterAndSize(st);
            LayoutChildren(st);
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            if (st && reinterpret_cast<HWND>(lParam) == st->hError) {
                SetTextColor(hdc, RGB(176, 0, 0));  // muted red for the warning
            }
            SetBkColor(hdc, GetSysColor(COLOR_WINDOW));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }

        case WM_COMMAND:
            if (st) {
                const int id = LOWORD(wParam);
                if (id == IDOK) {
                    Finish(st, /*cancelled=*/false);
                } else if (id == IDCANCEL) {
                    Finish(st, /*cancelled=*/true);
                }
            }
            return 0;

        case WM_CLOSE:
            if (st) Finish(st, /*cancelled=*/true);
            return 0;

        case WM_DPICHANGED:
            if (st) {
                st->dpi = HIWORD(wParam);
                auto* prc = reinterpret_cast<RECT*>(lParam);
                SetWindowPos(hwnd, nullptr, prc->left, prc->top,
                             prc->right - prc->left, prc->bottom - prc->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                ApplyFont(st);
                LayoutChildren(st);
            }
            return 0;

        case WM_DESTROY:
            if (st && st->hFont) {
                DeleteObject(st->hFont);
                st->hFont = nullptr;
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void EnsureClassRegistered(HINSTANCE inst) {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    registered = true;
}

}  // namespace

PasswordPromptResult ShowPasswordDialog(const std::wstring& archiveName,
                                        unsigned attempt, void* owner,
                                        void* hInstance) {
    HINSTANCE inst = static_cast<HINSTANCE>(hInstance);
    HWND ownerHwnd = static_cast<HWND>(owner);
    if (!inst) inst = GetModuleHandleW(nullptr);
    EnsureClassRegistered(inst);

    PwState st;
    st.archiveName = archiveName;
    st.attempt = attempt;

    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME, kClassName, kTitle,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT, ownerHwnd, nullptr, inst, &st);
    if (!hwnd) {
        return PasswordPromptResult{};  // cancelled by default (can't prompt)
    }

    // Modal: disable the owner and pump our own loop until OK/Cancel/close.
    if (ownerHwnd) EnableWindow(ownerHwnd, FALSE);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);
    SetFocus(st.hEdit);

    MSG msg;
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (ownerHwnd) {
        EnableWindow(ownerHwnd, TRUE);
        SetForegroundWindow(ownerHwnd);
    }
    DestroyWindow(hwnd);

    return st.result;
}

}  // namespace ae
