#include "archive_core/registration.h"

#include "archive_core/logging.h"

#include <format>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>    // SHChangeNotify, SHOpenWithDialog, OPENASINFO

namespace ae {
namespace {

// Base subkeys (relative to HKLM) for the two scopes we write under.
constexpr std::wstring_view kClassesBase = L"Software\\Classes\\";
constexpr std::wstring_view kSoftwareBase = L"Software\\";

std::wstring FullSubKey(RegScope scope, std::wstring_view subKey) {
    const std::wstring_view base =
        scope == RegScope::Classes ? kClassesBase : kSoftwareBase;
    return std::wstring(base) + std::wstring(subKey);
}

// Create-or-open the key and set one REG_SZ value (default value if name empty).
bool WriteValue(const RegEntry& e) {
    const std::wstring path = FullSubKey(e.scope, e.subKey);
    HKEY key = nullptr;
    LSTATUS rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr,
                                 REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                                 &key, nullptr);
    if (rc != ERROR_SUCCESS) {
        ae::Log(std::format(L"[register] create key failed ({}) : {}", rc, path));
        return false;
    }
    const wchar_t* name = e.valueName.empty() ? nullptr : e.valueName.c_str();
    // +1 for the terminating NUL; RegSetValueExW wants the byte count.
    const DWORD bytes =
        static_cast<DWORD>((e.valueData.size() + 1) * sizeof(wchar_t));
    rc = RegSetValueExW(key, name, 0, REG_SZ,
                        reinterpret_cast<const BYTE*>(e.valueData.c_str()), bytes);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) {
        ae::Log(std::format(L"[register] set value failed ({}) : {}\\{}", rc, path,
                            e.valueName));
        return false;
    }
    return true;
}

// Delete a key tree recursively. Returns true on success OR if it was already
// absent (idempotent unregister).
bool DeleteTree(RegScope scope, std::wstring_view subKey) {
    const std::wstring path = FullSubKey(scope, subKey);
    const LSTATUS rc =
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, path.c_str());
    if (rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND) return true;
    ae::Log(std::format(L"[unregister] delete failed ({}) : {}", rc, path));
    return false;
}

// Delete a single named value, leaving its key in place. Idempotent: a missing
// key or missing value is treated as success.
bool DeleteValue(const RegValueRef& v) {
    const std::wstring path = FullSubKey(v.scope, v.subKey);
    HKEY key = nullptr;
    const LSTATUS rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0,
                                     KEY_SET_VALUE, &key);
    if (rc == ERROR_FILE_NOT_FOUND) return true;  // key already gone
    if (rc != ERROR_SUCCESS) {
        ae::Log(std::format(L"[unregister] open key failed ({}) : {}", rc, path));
        return false;
    }
    const LSTATUS d = RegDeleteValueW(key, v.valueName.c_str());
    RegCloseKey(key);
    if (d == ERROR_SUCCESS || d == ERROR_FILE_NOT_FOUND) return true;
    ae::Log(std::format(L"[unregister] delete value failed ({}) : {}\\{}", d, path,
                        v.valueName));
    return false;
}

void BroadcastAssocChanged() {
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

}  // namespace

const std::vector<std::wstring>& RegistrationExtensions() {
    // The spec's exact list: the single-token formats plus the .tgz/.tbz2/.txz
    // compound shorthands. WITH leading dots (the form HKLM\Software\Classes
    // and Capabilities\FileAssociations want).
    static const std::vector<std::wstring> kExts = {
        L".zip", L".7z",  L".zst", L".rar", L".xz",  L".gz", L".lz4",
        L".tar", L".bz2", L".br",  L".tgz", L".tbz2", L".txz",
    };
    return kExts;
}

std::wstring CurrentExePath() {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
    if (n == 0 || n >= ARRAYSIZE(buf)) return std::wstring();
    return std::wstring(buf, n);
}

bool IsProcessElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation,
                                        sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

RegistryPlan BuildRegistryPlan(std::wstring_view exePath) {
    RegistryPlan plan;
    const std::wstring exe(exePath);
    const std::wstring progId(kProgId);

    // --- ProgID: HKLM\Software\Classes\ArchiveExtractor.Archive.1 -----------
    plan.entries.push_back(
        {RegScope::Classes, progId, L"", std::wstring(kProgIdFriendly)});
    plan.entries.push_back(
        {RegScope::Classes, progId + L"\\DefaultIcon", L"", exe + L",0"});
    plan.entries.push_back(
        {RegScope::Classes, progId + L"\\shell\\open\\command", L"",
         L"\"" + exe + L"\" \"%1\""});

    // --- Per-extension OpenWithProgids + Capabilities\FileAssociations ------
    const std::wstring capBase = L"ArchiveExtractor\\Capabilities";
    plan.entries.push_back(
        {RegScope::Software, capBase, L"ApplicationName", std::wstring(kAppName)});
    plan.entries.push_back({RegScope::Software, capBase, L"ApplicationDescription",
                            std::wstring(kAppDescription)});

    for (const std::wstring& ext : RegistrationExtensions()) {
        // HKLM\Software\Classes\.<ext>\OpenWithProgids has a NAMED VALUE whose
        // name is the ProgID and whose data is empty. (The shell reads the
        // ProgID list from the *value names* under OpenWithProgids, NOT from
        // subkeys — a subkey here would not advertise us in "Open with".)
        plan.entries.push_back(
            {RegScope::Classes, ext + L"\\OpenWithProgids", progId, L""});
        // Capabilities\FileAssociations\.<ext> = <ProgId>
        plan.entries.push_back(
            {RegScope::Software, capBase + L"\\FileAssociations", ext, progId});
    }

    // --- RegisteredApplications\<AppName> -> Capabilities key ---------------
    plan.entries.push_back({RegScope::Software, L"RegisteredApplications",
                            std::wstring(kAppName),
                            L"Software\\ArchiveExtractor\\Capabilities"});

    // --- Trees to remove on unregister --------------------------------------
    // The ProgID tree and the whole ArchiveExtractor capabilities tree, plus
    // the per-extension OpenWithProgids leaves and the RegisteredApplications
    // value.
    plan.keysToDelete.push_back({RegScope::Classes, progId});
    plan.keysToDelete.push_back({RegScope::Software, L"ArchiveExtractor"});
    for (const std::wstring& ext : RegistrationExtensions()) {
        // Remove only OUR ProgID VALUE under the extension's OpenWithProgids;
        // leave the OpenWithProgids key and the .<ext> key in place (other apps
        // may have their own ProgID values there).
        plan.valuesToDelete.push_back(
            {RegScope::Classes, ext + L"\\OpenWithProgids", progId});
    }
    // The RegisteredApplications value (a value under a shared key).
    plan.valuesToDelete.push_back(
        {RegScope::Software, L"RegisteredApplications", std::wstring(kAppName)});
    return plan;
}

RegResult Register() {
    if (!IsProcessElevated()) {
        ae::Log(L"[register] ERROR: requires elevation (administrator). HKLM "
                L"writes need an elevated process. Re-run from an elevated "
                L"prompt or via the installer.");
        return RegResult::NotElevated;
    }
    const std::wstring exe = CurrentExePath();
    if (exe.empty()) {
        ae::Log(L"[register] ERROR: could not resolve the executable path.");
        return RegResult::Failed;
    }
    const RegistryPlan plan = BuildRegistryPlan(exe);
    bool ok = true;
    for (const RegEntry& e : plan.entries) {
        if (!WriteValue(e)) ok = false;
    }
    BroadcastAssocChanged();
    if (!ok) {
        ae::Log(L"[register] completed with errors (see above).");
        return RegResult::Failed;
    }
    ae::Log(std::format(L"[register] OK — {} keys/values written for {}",
                        plan.entries.size(), exe));
    return RegResult::Ok;
}

RegResult Unregister() {
    if (!IsProcessElevated()) {
        ae::Log(L"[unregister] ERROR: requires elevation (administrator). HKLM "
                L"writes need an elevated process. Re-run from an elevated "
                L"prompt or via the installer.");
        return RegResult::NotElevated;
    }
    const RegistryPlan plan = BuildRegistryPlan(L"");  // exe path unused here
    bool ok = true;
    // Remove the ProgID + capabilities trees, then our individual named values
    // (OpenWithProgids ProgID entries + the RegisteredApplications value) whose
    // keys are shared with other apps and must survive.
    for (const auto& [scope, sub] : plan.keysToDelete) {
        if (!DeleteTree(scope, sub)) ok = false;
    }
    for (const RegValueRef& v : plan.valuesToDelete) {
        if (!DeleteValue(v)) ok = false;
    }
    BroadcastAssocChanged();
    if (!ok) {
        ae::Log(L"[unregister] completed with errors (see above).");
        return RegResult::Failed;
    }
    ae::Log(L"[unregister] OK — all Archive Extractor keys removed.");
    return RegResult::Ok;
}

bool PromptSetAsDefault(std::wstring_view filePath) {
    // The one-confirmation, user-driven default-handler path for OWNED
    // extensions (e.g. .zip). We never fabricate the UserChoice hash; the user
    // chooses us in the OS dialog. Per-user, so no elevation required.
    OPENASINFO info{};
    const std::wstring path(filePath);
    info.pcszFile = path.c_str();
    info.pcszClass = nullptr;
    info.oaifInFlags = OAIF_EXEC | OAIF_REGISTER_EXT;
    const HRESULT hr = SHOpenWithDialog(nullptr, &info);
    if (FAILED(hr)) {
        ae::Log(std::format(L"[default] SHOpenWithDialog hr=0x{:08x}",
                            static_cast<unsigned>(hr)));
        return false;
    }
    return true;
}

}  // namespace ae
