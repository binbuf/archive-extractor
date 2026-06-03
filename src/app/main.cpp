#include "archive_core/cli.h"
#include "archive_core/logging.h"
#include "archive_core/paths.h"
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

void ShowError(const std::wstring& text) {
    // Placeholder error UI — replaced by the real error dialog in task 06.
    MessageBoxW(nullptr, text.c_str(), L"Archive Extractor", MB_OK | MB_ICONERROR);
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
        ae::Log(std::format(L"[error] file not found: {}", cl.archivePath));
        ShowError(std::format(L"File not found:\n{}", cl.archivePath));
        return 2;
    }
    if (!ae::HasSupportedExtension(cl.archivePath)) {
        ae::Log(std::format(L"[error] unsupported file type: {}", cl.archivePath));
        ShowError(std::format(L"Unsupported file type:\n{}", cl.archivePath));
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
        default:
            ae::Log(std::format(L"[extract] failed: {}", dr.errorMessage));
            // Placeholder error UI until task 06 supplies the real dialog.
            ShowError(dr.errorMessage.empty()
                          ? std::wstring(L"Extraction failed.")
                          : dr.errorMessage);
            return 4;
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    AttachParentConsole();

    const HRESULT hr =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) {
        ShowError(L"Failed to initialize COM.");
        return 1;
    }

    int exitCode = 0;
    const ae::CommandLine cl = ae::ParseCommandLine(CollectArgs());

    switch (cl.mode) {
        case ae::Mode::Register:
            // Stub — real shell registration lands in task 08.
            ae::Log(L"[register] stub (no-op); returns success.");
            exitCode = 0;
            break;
        case ae::Mode::Unregister:
            // Stub — real shell unregistration lands in task 08.
            ae::Log(L"[unregister] stub (no-op); returns success.");
            exitCode = 0;
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
