#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace ae {

// --- Shell registration (task 08) -------------------------------------------
//
// Writes / removes the per-machine (HKLM) file-type registration so the shell
// launches Archive Extractor for the supported extensions and so the app shows
// up in *Open with* and *Default apps*.
//
// The logic is data-driven: BuildRegistryPlan() produces the complete set of
// keys/values WITHOUT touching the real registry (so it is unit-testable
// without elevation), and Register()/Unregister() apply/remove that plan under
// HKLM. The installer (task 13) calls the same CLI, so this is the single
// source of truth for the key set.
//
// All HKLM writes require elevation; Register()/Unregister() fail with a clear
// message when not elevated (see IsProcessElevated()).

// The ProgID that backs every supported type (the verb/command is identical
// for all of them).
inline constexpr std::wstring_view kProgId = L"ArchiveExtractor.Archive.1";

// Friendly ProgID description (default value of the ProgID key).
inline constexpr std::wstring_view kProgIdFriendly = L"Compressed Archive";

// Registered-Application display name (key under RegisteredApplications and the
// Capabilities\ApplicationName value).
inline constexpr std::wstring_view kAppName = L"Archive Extractor";

inline constexpr std::wstring_view kAppDescription =
    L"Extracts ZIP, 7z, RAR, TAR and other archives, macOS-style.";

// The canonical set of extensions we register, WITH the leading dot, in one
// shared constant reused by the installer. This is the registration source of
// truth.
//
// Reconciliation with detection: src/core/paths.cpp owns `kExts` (the bare,
// no-dot tokens used to GATE extraction on double-click). That set is a
// superset shorthand list (it also accepts .tbz, .tzst, .tlz4, .tlz). The
// registration set below is exactly the spec's list — the extensions we
// advertise to the shell — and every entry here is also accepted by detection,
// so a double-click of anything we register will extract.
const std::vector<std::wstring>& RegistrationExtensions();

// --- Registry key plan (pure data; no registry access) -----------------------

// One key+value to be written. `subKey` is relative to a root hive base:
//   scope == Classes  -> HKLM\Software\Classes\<subKey>
//   scope == Software -> HKLM\Software\<subKey>
// `valueName` empty means the key's default value. `valueData` is the REG_SZ
// payload (may be empty, e.g. the OpenWithProgids marker value).
enum class RegScope { Classes, Software };

struct RegEntry {
    RegScope scope = RegScope::Classes;
    std::wstring subKey;
    std::wstring valueName;  // empty => default value
    std::wstring valueData;
};

// A named value to delete on unregister (without removing the whole key, which
// other apps may share — notably the per-extension OpenWithProgids key).
struct RegValueRef {
    RegScope scope = RegScope::Classes;
    std::wstring subKey;
    std::wstring valueName;
};

// The complete registry plan. `entries` are the values to write (register) in
// order. `keysToDelete` are key trees removed by Unregister() (recursively).
// `valuesToDelete` are individual named values removed by Unregister() (the key
// is left in place because it may be shared with other applications).
struct RegistryPlan {
    std::vector<RegEntry> entries;
    // (scope, subKey) trees removed by Unregister(), recursively.
    std::vector<std::pair<RegScope, std::wstring>> keysToDelete;
    std::vector<RegValueRef> valuesToDelete;
};

// Build the full plan given the absolute path to the installed exe (used for
// the DefaultIcon and shell\open\command strings). Pure — no registry access,
// no elevation needed. Used by Register()/Unregister() and by the unit tests.
RegistryPlan BuildRegistryPlan(std::wstring_view exePath);

// --- Apply / remove ----------------------------------------------------------

enum class RegResult {
    Ok,
    NotElevated,  // HKLM write requires elevation; nothing was changed
    Failed,       // a registry operation failed (see the log for the key)
};

// True if the current process token is elevated (running as administrator).
bool IsProcessElevated();

// Full path to the running executable (GetModuleFileNameW). Empty on failure.
std::wstring CurrentExePath();

// Write the entire plan under HKLM (idempotent — re-running is safe) and
// broadcast SHCNE_ASSOCCHANGED. Fails with NotElevated when not elevated.
RegResult Register();

// Remove every key the plan created under HKLM and broadcast
// SHCNE_ASSOCCHANGED. Fails with NotElevated when not elevated.
RegResult Unregister();

// --- Owned-extension default-handler picker ----------------------------------
//
// Windows guards the *default* handler per extension behind a hashed UserChoice
// that third parties cannot set silently, so for an OWNED extension (notably
// .zip) we never fabricate it. Instead we let the USER pick us with one
// confirmation via the OS "Open with" dialog. This is the documented trigger
// the front end wires to a first-run / menu action.
//
// Shows the shell's Open-With dialog for `filePath` (SHOpenWithDialog) and
// registers the user's choice as the default handler for that file type WITHOUT
// re-opening the file. Returns true if the dialog was shown. Does NOT require
// elevation (it is a per-user choice). No-op-safe; intended as a one-confirmation
// path, not silent. Wired to the explicit `--set-default` CLI verb — the front
// end never offers this unprompted (a quick-extract tool shouldn't nag).
bool PromptSetAsDefault(std::wstring_view filePath);

}  // namespace ae
