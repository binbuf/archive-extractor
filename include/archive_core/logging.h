#pragma once

#include <string_view>

namespace ae {

// Emit one diagnostic line. Always written to the debugger via
// OutputDebugString; also echoed to stderr when a console/redirect is attached
// (a no-op otherwise). A trailing newline is appended.
void Log(std::wstring_view message);

}  // namespace ae
