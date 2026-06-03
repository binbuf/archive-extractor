#include "shell_reveal.h"

#include "archive_core/logging.h"
#include "archive_core/paths.h"

#include <format>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>     // CoInitializeEx / CoUninitialize
#include <shlobj.h>      // SHParseDisplayName, SHOpenFolderAndSelectItems, ILIsEqual
#include <shobjidl.h>    // IShellBrowser, IShellView, IFolderView, IPersistFolder2
#include <exdisp.h>      // IShellWindows, IWebBrowserApp
#include <shlwapi.h>     // path helpers
#include <oleauto.h>     // VariantInit / VariantClear

namespace ae {

namespace {

// --- Minimal RAII helpers ---------------------------------------------------

// Releases a COM interface pointer on scope exit.
template <typename T>
struct ComPtr {
    T* p = nullptr;
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ~ComPtr() {
        if (p) p->Release();
    }
    T** put() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
    void reset() {
        if (p) {
            p->Release();
            p = nullptr;
        }
    }
};

// Frees an absolute PIDL with CoTaskMemFree on scope exit.
struct Pidl {
    PIDLIST_ABSOLUTE p = nullptr;
    Pidl() = default;
    Pidl(const Pidl&) = delete;
    Pidl& operator=(const Pidl&) = delete;
    ~Pidl() {
        if (p) CoTaskMemFree(p);
    }
    PIDLIST_ABSOLUTE* put() { return &p; }
    PIDLIST_ABSOLUTE get() const { return p; }
    PIDLIST_ABSOLUTE release() {
        PIDLIST_ABSOLUTE t = p;
        p = nullptr;
        return t;
    }
    explicit operator bool() const { return p != nullptr; }
};

// Parse a filesystem path into an absolute PIDL. Returns false on failure.
bool ParsePidl(const std::wstring& path, Pidl& out) {
    return SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, out.put(), 0,
                                        nullptr));
}

// Bring a window (and its top-level owner) to the foreground.
void Foreground(HWND hwnd) {
    if (!hwnd) return;
    HWND top = GetAncestor(hwnd, GA_ROOT);
    if (!top) top = hwnd;
    if (IsIconic(top)) ShowWindow(top, SW_RESTORE);
    SetForegroundWindow(top);
}

// Select `itemPidl` inside the shell view of an already-open Explorer window
// (given its IShellBrowser), clearing any prior selection. Returns true on
// success.
bool SelectInBrowser(IShellBrowser* browser, PCIDLIST_ABSOLUTE folderPidl,
                     PCIDLIST_ABSOLUTE itemPidl) {
    ComPtr<IShellView> view;
    if (FAILED(browser->QueryActiveShellView(view.put())) || !view) return false;

    // The relative (child) PIDL of the item within its parent folder.
    PCITEMID_CHILD child = ILFindLastID(itemPidl);
    if (!child) return false;
    (void)folderPidl;

    // Deselect everything, then select + focus the new item.
    view->SelectItem(nullptr, SVSI_DESELECTOTHERS);
    HRESULT hr = view->SelectItem(
        child, SVSI_SELECT | SVSI_FOCUSED | SVSI_ENSUREVISIBLE);
    return SUCCEEDED(hr);
}

// Try to reuse an already-open Explorer window that is displaying `folderPidl`.
// On success selects `itemPidl` there, brings the window to front, and returns
// true. Returns false if no matching window exists (caller falls back).
bool TryReuseWindow(PCIDLIST_ABSOLUTE folderPidl, PCIDLIST_ABSOLUTE itemPidl) {
    ComPtr<IShellWindows> shellWindows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(shellWindows.put()))) ||
        !shellWindows) {
        return false;
    }

    long count = 0;
    if (FAILED(shellWindows->get_Count(&count))) return false;

    for (long i = 0; i < count; ++i) {
        VARIANT vi;
        VariantInit(&vi);
        vi.vt = VT_I4;
        vi.lVal = i;

        ComPtr<IDispatch> disp;
        HRESULT hr = shellWindows->Item(vi, disp.put());
        VariantClear(&vi);
        if (FAILED(hr) || !disp) continue;

        // A window in IShellWindows may be an Explorer (filesystem) view or an
        // Internet Explorer window. We only want filesystem views, reachable via
        // IShellBrowser -> IShellView -> IFolderView -> folder PIDL.
        ComPtr<IWebBrowserApp> wba;
        if (FAILED(disp->QueryInterface(IID_PPV_ARGS(wba.put()))) || !wba) {
            continue;
        }

        ComPtr<IServiceProvider> sp;
        if (FAILED(wba->QueryInterface(IID_PPV_ARGS(sp.put()))) || !sp) continue;

        ComPtr<IShellBrowser> browser;
        if (FAILED(sp->QueryService(SID_STopLevelBrowser,
                                    IID_PPV_ARGS(browser.put()))) ||
            !browser) {
            continue;
        }

        ComPtr<IShellView> view;
        if (FAILED(browser->QueryActiveShellView(view.put())) || !view) continue;

        ComPtr<IFolderView> folderView;
        if (FAILED(view->QueryInterface(IID_PPV_ARGS(folderView.put()))) ||
            !folderView) {
            continue;
        }

        ComPtr<IPersistFolder2> persist;
        if (FAILED(folderView->GetFolder(IID_PPV_ARGS(persist.put()))) ||
            !persist) {
            continue;
        }

        Pidl curFolder;
        if (FAILED(persist->GetCurFolder(curFolder.put())) || !curFolder) {
            continue;
        }

        // Compare via PIDL identity — robust against case / trailing-slash /
        // short-vs-long-name differences in the textual path.
        if (!ILIsEqual(curFolder.get(), folderPidl)) continue;

        // Match: select the item in this existing window and raise it.
        if (!SelectInBrowser(browser.p, folderPidl, itemPidl)) {
            // Even if selection failed, we found the window; bring it forward.
            Log(L"[reveal] reuse: select failed; raising window anyway");
        }

        HWND hwnd = nullptr;
        if (SUCCEEDED(wba->get_HWND(reinterpret_cast<SHANDLE_PTR*>(&hwnd))) &&
            hwnd) {
            Foreground(hwnd);
        }
        Log(L"[reveal] reused existing Explorer window for target folder");
        return true;
    }
    return false;
}

// Fallback: open (or focus) a window on `folderPidl` with `itemPidl` selected.
bool OpenAndSelect(PCIDLIST_ABSOLUTE folderPidl, PCIDLIST_ABSOLUTE itemPidl) {
    PCITEMID_CHILD items[1] = {ILFindLastID(itemPidl)};
    HRESULT hr = SHOpenFolderAndSelectItems(folderPidl, 1, items, 0);
    if (FAILED(hr)) {
        // As a last resort, open the folder with no selection.
        hr = SHOpenFolderAndSelectItems(folderPidl, 0, nullptr, 0);
    }
    if (SUCCEEDED(hr)) {
        Log(L"[reveal] opened/focused Explorer via SHOpenFolderAndSelectItems");
        return true;
    }
    Log(std::format(L"[reveal] SHOpenFolderAndSelectItems failed hr=0x{:08x}",
                    static_cast<unsigned>(hr)));
    return false;
}

}  // namespace

bool RevealInExplorer(const std::wstring& placedPath) {
    if (placedPath.empty()) {
        Log(L"[reveal] nothing to reveal (empty archive)");
        return false;
    }

    // Ensure COM (STA) on this thread. The UI thread already initialized it at
    // startup, so this typically returns S_FALSE (already initialized); we only
    // CoUninitialize the count we added.
    const HRESULT coHr =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool weInitialized = SUCCEEDED(coHr) && coHr != S_FALSE;

    bool revealed = false;
    {
        const std::wstring folder = ParentDirectory(placedPath);

        Pidl folderPidl;
        Pidl itemPidl;
        if (ParsePidl(folder, folderPidl) && ParsePidl(placedPath, itemPidl) &&
            folderPidl && itemPidl) {
            Log(std::format(L"[reveal] target item: {}", placedPath));
            if (TryReuseWindow(folderPidl.get(), itemPidl.get())) {
                revealed = true;
            } else {
                revealed = OpenAndSelect(folderPidl.get(), itemPidl.get());
            }
        } else {
            Log(std::format(L"[reveal] failed to parse PIDL for: {}", placedPath));
        }
    }  // RAII frees PIDLs / COM interfaces before CoUninitialize below.

    if (weInitialized) CoUninitialize();
    return revealed;
}

}  // namespace ae
