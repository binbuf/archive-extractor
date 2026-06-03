#include "progress_dialog.h"

#include "archive_core/detect.h"
#include "archive_core/error_model.h"
#include "archive_core/extract.h"
#include "archive_core/layout.h"
#include "archive_core/logging.h"
#include "archive_core/paths.h"
#include "shell_reveal.h"

#include <atomic>
#include <format>
#include <memory>
#include <thread>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <shlwapi.h>  // PathFindFileNameW

namespace ae {

namespace {

// ---------------------------------------------------------------------------
// Worker -> UI message protocol (all posted via PostMessage from the worker).
// ---------------------------------------------------------------------------
// Throttled progress sample. lParam = new Progress* (heap, UI thread deletes).
constexpr UINT WM_APP_PROGRESS = WM_APP + 1;
// Flow finished. lParam = new WorkerResult* (heap, UI thread deletes).
constexpr UINT WM_APP_DONE = WM_APP + 2;

// Flash-suppression threshold. Extractions that finish faster than this never
// show the window, avoiding a flicker on trivial archives. Chosen at 200 ms per
// the design spec (§Extraction dialog: "<~200 ms ... suppressed until a short
// delay elapses").
constexpr UINT kFlashSuppressMs = 200;
constexpr UINT_PTR kShowTimerId = 1;

const wchar_t* kClassName = L"ae.ExtractionProgressDialog";

// Logical (DPI-unscaled) dialog metrics; scaled per-monitor at create time.
constexpr int kDlgWidthDip = 420;
constexpr int kDlgHeightDip = 110;
constexpr int kMarginDip = 16;
constexpr int kLabelHeightDip = 20;
constexpr int kBarHeightDip = 22;
constexpr int kBtnWidthDip = 84;
constexpr int kBtnHeightDip = 26;
constexpr int kGapDip = 12;

// Result the worker hands back to the UI thread on completion.
struct WorkerResult {
    DialogOutcome outcome = DialogOutcome::Failed;
    ErrorInfo error;          // populated on failure (code + message)
    std::wstring revealPath;  // placed top-level item (success only)
};

// ---------------------------------------------------------------------------
// Per-window state. Lives on the UI thread; the worker only touches the atomic
// cancellation token and posts messages.
// ---------------------------------------------------------------------------
struct DialogState {
    std::wstring archivePath;
    std::wstring fileName;  // leaf with extension, for the label

    HWND hwnd = nullptr;
    HWND hLabel = nullptr;
    HWND hBar = nullptr;
    HWND hCancel = nullptr;
    HFONT hFont = nullptr;

    CancellationToken cancel;
    std::thread worker;

    bool shown = false;       // window currently visible
    bool finished = false;    // WM_APP_DONE received
    bool determinate = false;  // a progress sample with total>0 arrived
    bool cancelRequested = false;

    UINT dpi = 96;
    DialogResult result;  // filled on completion; returned by the run loop
};

int Scale(int dip, UINT dpi) {
    return MulDiv(dip, static_cast<int>(dpi), 96);
}

// Pick the monitor under the cursor (active monitor), else the primary, and
// return its work area for centering.
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

// Create a themed UI font sized for the given DPI.
HFONT MakeUiFont(UINT dpi) {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0,
                                   dpi)) {
        return CreateFontIndirectW(&ncm.lfMessageFont);
    }
    return static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

void LayoutChildren(DialogState* st) {
    const UINT dpi = st->dpi;
    const int margin = Scale(kMarginDip, dpi);
    const int gap = Scale(kGapDip, dpi);
    const int labelH = Scale(kLabelHeightDip, dpi);
    const int barH = Scale(kBarHeightDip, dpi);
    const int btnW = Scale(kBtnWidthDip, dpi);
    const int btnH = Scale(kBtnHeightDip, dpi);

    RECT rc;
    GetClientRect(st->hwnd, &rc);
    const int cw = rc.right - rc.left;

    const int contentW = cw - 2 * margin;
    int y = margin;
    SetWindowPos(st->hLabel, nullptr, margin, y, contentW, labelH,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    y += labelH + gap;

    const int barRowH = barH > btnH ? barH : btnH;
    const int barW = contentW - btnW - gap;
    const int barY = y + (barRowH - barH) / 2;
    const int btnY = y + (barRowH - btnH) / 2;
    SetWindowPos(st->hBar, nullptr, margin, barY, barW, barH,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(st->hCancel, nullptr, margin + barW + gap, btnY, btnW, btnH,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

// Set the (ellipsis-truncated) label text. SS_ENDELLIPSIS on the static does
// the visual truncation; we just supply the full string.
void SetLabel(DialogState* st) {
    const std::wstring text = std::format(L"Expanding \"{}\"", st->fileName);
    SetWindowTextW(st->hLabel, text.c_str());
}

void CenterAndSize(DialogState* st) {
    const UINT dpi = st->dpi;
    const int w = Scale(kDlgWidthDip, dpi);
    const int h = Scale(kDlgHeightDip, dpi);
    const RECT work = ActiveMonitorRect();
    const int x = work.left + ((work.right - work.left) - w) / 2;
    const int y = work.top + ((work.bottom - work.top) - h) / 2;
    SetWindowPos(st->hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

void ApplyFont(DialogState* st) {
    if (st->hFont) DeleteObject(st->hFont);
    st->hFont = MakeUiFont(st->dpi);
    auto setFont = [&](HWND h) {
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(st->hFont), TRUE);
    };
    setFont(st->hLabel);
    setFont(st->hCancel);
}

// Switch the bar to marquee (indeterminate) animation.
void StartMarquee(DialogState* st) {
    LONG_PTR style = GetWindowLongPtrW(st->hBar, GWL_STYLE);
    SetWindowLongPtrW(st->hBar, GWL_STYLE, style | PBS_MARQUEE);
    SendMessageW(st->hBar, PBM_SETMARQUEE, TRUE, 30);
}

// Switch the bar to determinate mode (0..1000 range, smooth-ish).
void StartDeterminate(DialogState* st) {
    SendMessageW(st->hBar, PBM_SETMARQUEE, FALSE, 0);
    LONG_PTR style = GetWindowLongPtrW(st->hBar, GWL_STYLE);
    SetWindowLongPtrW(st->hBar, GWL_STYLE, style & ~PBS_MARQUEE);
    SendMessageW(st->hBar, PBM_SETRANGE32, 0, 1000);
    st->determinate = true;
}

// Trigger cancellation: flag the worker, stop the show timer, and let the
// worker abort + clean temp; the window closes when WM_APP_DONE arrives.
void RequestCancel(DialogState* st) {
    if (st->cancelRequested || st->finished) return;
    st->cancelRequested = true;
    st->cancel.Cancel();
    KillTimer(st->hwnd, kShowTimerId);
    EnableWindow(st->hCancel, FALSE);
}

void ShowDialogWindow(DialogState* st) {
    if (st->shown || st->finished) return;
    st->shown = true;
    ShowWindow(st->hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(st->hwnd);
}

// ---------------------------------------------------------------------------
// The orchestrated flow, run entirely on the worker thread. Posts progress and
// a single completion message back to the UI window. Owns NOTHING on the UI
// thread except the posted heap payloads.
// ---------------------------------------------------------------------------
void WorkerFlow(DialogState* st) {
    const std::wstring& archivePath = st->archivePath;
    const std::wstring workingDir = ParentDirectory(archivePath);

    auto postDone = [&](WorkerResult&& wr) {
        PostMessageW(st->hwnd, WM_APP_DONE, 0,
                     reinterpret_cast<LPARAM>(new WorkerResult(std::move(wr))));
    };

    // Build a failure WorkerResult from the unified error model and log the
    // code + format (never contents/passwords) before posting it.
    auto fail = [&](ErrorCode code, Format format,
                    std::wstring_view detail = std::wstring_view()) {
        WorkerResult wr;
        wr.outcome = DialogOutcome::Failed;
        wr.error = MakeError(code, st->fileName);
        Log(std::format(L"[error] code={} format={} archive={}{}",
                        ErrorCodeToken(code), static_cast<int>(format),
                        st->fileName,
                        detail.empty() ? std::wstring()
                                       : std::format(L" detail={}", detail)));
        postDone(std::move(wr));
    };

    auto cancelled = [&]() {
        postDone(WorkerResult{DialogOutcome::Cancelled, ErrorInfo{}, L""});
    };

    // --- Detect (02) -------------------------------------------------------
    PipelinePlan plan = DetectFile(archivePath);
    if (!plan.supported() || plan.backend != Backend::LibArchive) {
        // Out of scope: encrypted-7z/RAR (SevenZipDll) and brotli backends land
        // in later tasks. Surface as the catalog "unsupported" condition.
        fail(ErrorCodeFromPlan(plan), plan.format, L"format/backend unsupported");
        return;
    }

    if (st->cancel.IsCancelled()) {
        cancelled();
        return;
    }

    // --- Staging dir (03) --------------------------------------------------
    const std::wstring stagingDir = CreateStagingDir(workingDir);
    if (stagingDir.empty()) {
        fail(ErrorCode::Internal, plan.format, L"CreateStagingDir failed");
        return;
    }

    // --- Extract into staging (03) -----------------------------------------
    ExtractCallbacks cb;
    cb.cancel = &st->cancel;
    HWND hwnd = st->hwnd;
    cb.onProgress = [hwnd](const Progress& p) {
        // Marshal a copy to the UI thread; the UI deletes it.
        PostMessageW(hwnd, WM_APP_PROGRESS, 0,
                     reinterpret_cast<LPARAM>(new Progress(p)));
    };
    // Password hook is task 10 — pass nothing (engine treats null as "no
    // password" and surfaces NeedPassword if it hits an encrypted entry).

    LibarchiveExtractor extractor;
    const std::wstring sourcePath =
        plan.firstPartPath.empty() ? archivePath : plan.firstPartPath;
    ExtractResult er =
        extractor.extractToStaging(plan, sourcePath, stagingDir, cb);

    if (er.status == ExtractStatus::Cancelled) {
        RemoveDirTree(stagingDir);  // defensive: clean temp on cancel
        Log(L"[extract] cancelled; temp removed");
        cancelled();
        return;
    }
    if (er.status != ExtractStatus::Ok) {
        RemoveDirTree(stagingDir);  // fail clean: no partial output left behind
        fail(ErrorCodeFromExtract(er.status), plan.format, er.message);
        return;
    }

    if (st->cancel.IsCancelled()) {
        RemoveDirTree(stagingDir);
        cancelled();
        return;
    }

    // --- Place (04) --------------------------------------------------------
    LayoutPlanner planner;
    PlaceResult pr = planner.place(stagingDir, workingDir, plan.stem, plan.kind);

    if (pr.status == PlaceStatus::NoOutput) {
        // Empty archive: nothing placed, completes silently. Staging already
        // removed by the planner. Success with nothing to reveal.
        Log(L"[place] empty archive; nothing placed");
        WorkerResult wr;
        wr.outcome = DialogOutcome::Success;
        postDone(std::move(wr));
        return;
    }
    if (pr.status != PlaceStatus::Ok) {
        RemoveDirTree(stagingDir);  // planner removes on error, but be safe
        fail(ErrorCodeFromPlace(pr.status, pr.lastError), plan.format,
             pr.message);
        return;
    }

    Log(std::format(L"[place] ok: {} (renamed={}, roots={})", pr.placedPath,
                    pr.renamed, pr.rootEntryCount));

    WorkerResult wr;
    wr.outcome = DialogOutcome::Success;
    wr.revealPath = pr.placedPath;
    postDone(std::move(wr));
}

// ---------------------------------------------------------------------------
// Window procedure (UI thread).
// ---------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DialogState* st = reinterpret_cast<DialogState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            st = reinterpret_cast<DialogState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(st));
            st->hwnd = hwnd;
            st->dpi = GetDpiForWindow(hwnd);

            HINSTANCE inst = cs->hInstance;
            st->hLabel = CreateWindowExW(
                0, L"STATIC", L"",
                WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX,
                0, 0, 0, 0, hwnd, nullptr, inst, nullptr);
            // Marquee by default; switched to determinate on the first sized
            // progress sample. PBS_SMOOTH for a continuous-looking fill.
            st->hBar = CreateWindowExW(
                0, PROGRESS_CLASSW, L"",
                WS_CHILD | WS_VISIBLE | PBS_MARQUEE | PBS_SMOOTH, 0, 0, 0, 0,
                hwnd, nullptr, inst, nullptr);
            st->hCancel = CreateWindowExW(
                0, L"BUTTON", L"Cancel",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0,
                hwnd, reinterpret_cast<HMENU>(IDCANCEL), inst, nullptr);

            ApplyFont(st);
            SetLabel(st);
            CenterAndSize(st);
            LayoutChildren(st);
            StartMarquee(st);

            // Flash suppression: start hidden; reveal only if the extraction is
            // still running after kFlashSuppressMs (200 ms). If WM_APP_DONE
            // arrives first, we never show.
            SetTimer(hwnd, kShowTimerId, kFlashSuppressMs, nullptr);
            return 0;
        }

        case WM_TIMER:
            if (wParam == kShowTimerId) {
                KillTimer(hwnd, kShowTimerId);
                ShowDialogWindow(st);
            }
            return 0;

        case WM_APP_PROGRESS: {
            std::unique_ptr<Progress> p(reinterpret_cast<Progress*>(lParam));
            if (st && !st->finished) {
                if (p->total > 0) {
                    if (!st->determinate) StartDeterminate(st);
                    // Map done/total onto the 0..1000 range.
                    const double frac =
                        static_cast<double>(p->done) /
                        static_cast<double>(p->total);
                    int pos = static_cast<int>(frac * 1000.0 + 0.5);
                    if (pos < 0) pos = 0;
                    if (pos > 1000) pos = 1000;
                    SendMessageW(st->hBar, PBM_SETPOS, pos, 0);
                }
                // total==0 -> stay on marquee (indeterminate); nothing to do.
            }
            return 0;
        }

        case WM_APP_DONE: {
            std::unique_ptr<WorkerResult> wr(
                reinterpret_cast<WorkerResult*>(lParam));
            if (st) {
                st->finished = true;
                KillTimer(hwnd, kShowTimerId);
                st->result.outcome = wr->outcome;
                st->result.error = wr->error;
                if (wr->outcome == DialogOutcome::Success) {
                    // Runs on the UI thread, which has COM initialized as an STA
                    // (CoInitializeEx APARTMENTTHREADED at startup) — the correct
                    // thread for IShellWindows / SHOpenFolderAndSelectItems.
                    RevealInExplorer(wr->revealPath);
                }
            }
            DestroyWindow(hwnd);
            return 0;
        }

        case WM_COMMAND:
            if (LOWORD(wParam) == IDCANCEL) {
                RequestCancel(st);
            }
            return 0;

        case WM_CLOSE:  // the X button
            RequestCancel(st);
            return 0;    // do NOT destroy yet; wait for the worker to finish

        case WM_DPICHANGED: {
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
        }

        case WM_SIZE:
            if (st && st->hBar) LayoutChildren(st);
            return 0;

        case WM_CTLCOLORSTATIC:
            // Default themed background for the label.
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));

        case WM_DESTROY:
            if (st && st->hFont) {
                DeleteObject(st->hFont);
                st->hFont = nullptr;
            }
            PostQuitMessage(0);
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

DialogResult RunExtractionDialog(const std::wstring& archivePath,
                                 void* hInstance) {
    HINSTANCE inst = static_cast<HINSTANCE>(hInstance);

    // Common controls (progress bar / button) from the v6 manifest.
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    EnsureClassRegistered(inst);

    DialogState st;
    st.archivePath = archivePath;
    st.fileName = PathFindFileNameW(archivePath.c_str());

    // No min/max box; a thin tool-window-ish frame. WS_EX_DLGMODALFRAME for a
    // clean dialog border; not actually modal (own message loop, modeless).
    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_APPWINDOW, kClassName,
        L"Archive Extractor",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr, inst, &st);
    if (!hwnd) {
        DialogResult r;
        r.outcome = DialogOutcome::Failed;
        r.error = MakeError(ErrorCode::Internal, st.fileName);
        return r;
    }

    // Launch the worker only once the window exists (it posts to st.hwnd).
    st.worker = std::thread(WorkerFlow, &st);

    // UI message pump. Handles WM_APP_PROGRESS / WM_APP_DONE; WM_DESTROY posts
    // WM_QUIT which ends the loop. IsDialogMessageW translates Esc -> IDCANCEL
    // (delivered as WM_COMMAND, handled as Cancel) and gives tab navigation /
    // the Cancel button keyboard activation.
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    // The window is gone; join the worker (it has already posted DONE and
    // returned, or is about to). This guarantees no dangling reference to `st`.
    if (st.worker.joinable()) st.worker.join();

    return st.result;
}

}  // namespace ae
