#pragma once

#include <string>
#include <string_view>

namespace ae {

// The working directory for layout decisions is the directory the archive lives
// in — derived from the archive path, never the process CWD (unreliable when
// launched by the shell). Returns the parent directory of `path`.
std::wstring ParentDirectory(std::wstring_view path);

// True if the filename ends in a supported archive extension — single (.zip,
// .7z, .br, .tar, ...) or compound abbreviation (.tgz, .txz, ...). The trailing
// token of a compound like ".tar.gz" matches via its final extension (".gz").
// Case-insensitive.
bool HasSupportedExtension(std::wstring_view path);

// True if a filesystem entry exists at `path`.
bool PathExists(std::wstring_view path);

}  // namespace ae
