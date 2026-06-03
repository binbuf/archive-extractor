#include "archive_core/cli.h"
#include "archive_core/error_model.h"
#include "archive_core/logging.h"
#include "archive_core/paths.h"
#include "archive_core/registration.h"
#include "error_dialog.h"
#include "progress_dialog.h"

#include <cstdio>
#include <format>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>   // CoInitializeEx / CoUninitialize
#include <shellapi.h>  // CommandLineToArgvW
#include <shlwapi.h>   // PathFindFileNameW

namespace {

// Tokenize the wide command line (wWinMain only hands us the raw tail and an
// ANSI lpCmdLine; CommandLineToArgvW gives correct UTF-16 argv splitting).
std::vector<std::wstring> CollectArgs() {
    std::vector<std::wstring> args;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    LocalFree(argv);
    return args;
}

// Leaf name (with extension) for naming the archive in error messages.
std::wstring LeafName(const std::wstring& path) {
    return PathFindFileNameW(path.c_str());
}

// Show the unified error dialog (task 06) for a catalog code + archive name.
void ShowError(ae::ErrorCode code, const std::wstring& archivePath) {
    ae::ShowErrorDialog(ae::MakeError(code, LeafName(archivePath)));
}

// Make Log() output visible when launched from a terminal, while respecting any
// explicit redirection. If a standard stream is already bound to a file or pipe
// (e.g. `ArchiveExtractor.exe ... > log.txt`), leave it alone; otherwise attach
// to the launching console. Harmless when double-clicked from the shell (no
// parent console — AttachConsole simply fails).
void AttachParentConsole() {
    auto isRedirected = [](DWORD stdHandle) {
        const DWORD type = GetFileType(GetStdHandle(stdHandle));
        return type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE;
    };
    const bool outRedirected = isRedirected(STD_OUTPUT_HANDLE);
    const bool errRedirected = isRedirected(STD_ERROR_HANDLE);
    if (outRedirected && errRedirected) return;

    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        if (!outRedirected) freopen_s(&f, "CONOUT$", "w", stdout);
        if (!errRedirected) freopen_s(&f, "CONOUT$", "w", stderr);
    }
}

int RunExtract(const ae::CommandLine& cl, HINSTANCE hInstance) {
    if (!ae::PathExists(cl.archivePath)) {
        ae::Log(std::format(L"[error] code={} archive={}",
                            ae::ErrorCodeToken(ae::ErrorCode::FileNotFound),
                            LeafName(cl.archivePath)));
        ShowError(ae::ErrorCode::FileNotFound, cl.archivePath);
        return 2;
    }
    if (!ae::HasSupportedExtension(cl.archivePath)) {
        ae::Log(std::format(L"[error] code={} archive={}",
                            ae::ErrorCodeToken(ae::ErrorCode::Unsupported),
                            LeafName(cl.archivePath)));
        ShowError(ae::ErrorCode::Unsupported, cl.archivePath);
        return 3;
    }

    const std::wstring workingDir = ae::ParentDirectory(cl.archivePath);
    ae::Log(std::format(L"[extract] archive     : {}", cl.archivePath));
    ae::Log(std::format(L"[extract] working dir : {}", workingDir));

    // Run the full flow (detect -> stage+extract -> place -> reveal stub)
    // behind the progress dialog. All extraction work happens on a worker
    // thread; this call pumps the UI loop and returns when the window closes.
    const ae::DialogResult dr =
        ae::RunExtractionDialog(cl.archivePath, hInstance);

    switch (dr.outcome) {
        case ae::DialogOutcome::Success:
            ae::Log(L"[extract] done");
            return 0;
        case ae::DialogOutcome::Cancelled:
            ae::Log(L"[extract] cancelled by user");
            return 0;  // user cancellation is not an error exit
        case ae::DialogOutcome::Failed:
        default: {
            // The worker already logged the code/format; show the real error
            // dialog from the unified error model (temp staging removed by the
            // flow — no partial output remains).
            ae::ErrorInfo info = dr.error;
            if (!info.isError()) {
                info = ae::MakeError(ae::ErrorCode::Internal,
                                     LeafName(cl.archivePath));
            }
            ae::ShowErrorDialog(info);
            return 4;
        }
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    AttachParentConsole();
    // Rotating diagnostic log at %LOCALAPPDATA%\ArchiveExtractor\log.txt. Records
    // format / error code / failing entry; never archive contents or passwords.
    ae::InitFileLog();

    const HRESULT hr =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) {
        ae::ShowErrorDialog(ae::MakeError(ae::ErrorCode::Internal, L""));
        return 1;
    }

    int exitCode = 0;
    const ae::CommandLine cl = ae::ParseCommandLine(CollectArgs());

    switch (cl.mode) {
        case ae::Mode::Register:
            // Write the per-machine (HKLM) shell registration. Requires
            // elevation; on a non-elevated process this fails cleanly with a
            // clear message and a nonzero exit. The installer (task 13) invokes
            // this same path.
            exitCode = (ae::Register() == ae::RegResult::Ok) ? 0 : 5;
            break;
        case ae::Mode::Unregister:
            // Remove everything --register wrote (idempotent). Requires
            // elevation just like --register.
            exitCode = (ae::Unregister() == ae::RegResult::Ok) ? 0 : 5;
            break;
        case ae::Mode::SetDefault:
            // User-driven default-handler picker for OWNED extensions (e.g.
            // .zip). We never fabricate the UserChoice hash; this opens the OS
            // "Open with" dialog so the user confirms us as default. Per-user,
            // no elevation. Wired here as the documented first-run/menu trigger.
            ae::Log(L"[default] opening OS Open-With picker.");
            exitCode = ae::PromptSetAsDefault(cl.archivePath) ? 0 : 6;
            break;
        case ae::Mode::Extract:
            exitCode = RunExtract(cl, hInstance);
            break;
        case ae::Mode::None:
        default:
            // No archive given is a no-op, not an error (design §16).
            ae::Log(L"[info] no archive specified; nothing to do.");
            exitCode = 0;
            break;
    }

    CoUninitialize();
    return exitCode;
}
