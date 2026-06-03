#include "archive_core/logging.h"

#include <cstdio>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace ae {

void Log(std::wstring_view message) {
    std::wstring line(message);
    line.push_back(L'\n');
    OutputDebugStringW(line.c_str());
    // Best-effort console echo; silently fails if no console is attached.
    std::fputws(line.c_str(), stderr);
}

}  // namespace ae
