#include "archive_core/error_model.h"

#include <format>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace ae {

// --- Catalog -----------------------------------------------------------------

std::wstring ErrorMessageFor(ErrorCode code, std::wstring_view archiveName) {
    const std::wstring name(archiveName);
    // Every message opens with the same "Couldn't expand '<name>'." stem (per
    // the UI spec example wording) followed by a plain-language cause. Cancel /
    // None has no message (it is never shown as an error).
    auto couldnt = [&](std::wstring_view why) -> std::wstring {
        return std::format(L"Couldn't expand “{}”. {}", name, why);
    };

    switch (code) {
        case ErrorCode::None:
            return std::wstring();
        case ErrorCode::Unsupported:
            return couldnt(L"This type of file isn't supported.");
        case ErrorCode::Corrupt:
            return couldnt(L"The archive appears to be corrupt.");
        case ErrorCode::Encryption:
            return couldnt(
                L"Its encryption isn't supported.");
        case ErrorCode::NeedPassword:
            return couldnt(L"It is password protected.");
        case ErrorCode::DiskFull:
            return couldnt(L"There isn't enough disk space.");
        case ErrorCode::AccessDenied:
            return couldnt(L"You don't have permission to write to this folder.");
        case ErrorCode::PathTooLong:
            return couldnt(L"A file path inside it is too long.");
        case ErrorCode::FileInUse:
            return couldnt(L"A file is in use by another program.");
        case ErrorCode::Untrusted:
            return couldnt(
                L"It contains an unsafe path and may be malicious.");
        case ErrorCode::FileNotFound:
            return couldnt(L"The file could no longer be found.");
        case ErrorCode::WriteFailed:
            return couldnt(L"A file could not be written.");
        case ErrorCode::PlaceFailed:
            return couldnt(L"The extracted files could not be placed.");
        case ErrorCode::Internal:
            return couldnt(L"An unexpected error occurred.");
    }
    return couldnt(L"An unexpected error occurred.");
}

const wchar_t* ErrorCodeToken(ErrorCode code) {
    switch (code) {
        case ErrorCode::None:         return L"NONE";
        case ErrorCode::Unsupported:  return L"UNSUPPORTED";
        case ErrorCode::Corrupt:      return L"CORRUPT";
        case ErrorCode::Encryption:   return L"ENCRYPTION";
        case ErrorCode::NeedPassword: return L"NEED_PASSWORD";
        case ErrorCode::DiskFull:     return L"DISK_FULL";
        case ErrorCode::AccessDenied: return L"ACCESS_DENIED";
        case ErrorCode::PathTooLong:  return L"PATH_TOO_LONG";
        case ErrorCode::FileInUse:    return L"FILE_IN_USE";
        case ErrorCode::Untrusted:    return L"UNTRUSTED";
        case ErrorCode::FileNotFound: return L"FILE_NOT_FOUND";
        case ErrorCode::WriteFailed:  return L"WRITE_FAILED";
        case ErrorCode::PlaceFailed:  return L"PLACE_FAILED";
        case ErrorCode::Internal:     return L"INTERNAL";
    }
    return L"UNKNOWN";
}

ErrorInfo MakeError(ErrorCode code, std::wstring_view archiveName) {
    ErrorInfo info;
    info.code = code;
    info.archiveName.assign(archiveName);
    info.message = ErrorMessageFor(code, archiveName);
    return info;
}

// --- Win32 refinement --------------------------------------------------------

ErrorCode RefineWin32WriteError(unsigned long lastError, ErrorCode fallback) {
    switch (lastError) {
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL:
            return ErrorCode::DiskFull;
        case ERROR_ACCESS_DENIED:
        case ERROR_WRITE_PROTECT:
            return ErrorCode::AccessDenied;
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
            return ErrorCode::FileInUse;
        case ERROR_FILENAME_EXCED_RANGE:
        case ERROR_BUFFER_OVERFLOW:
        case ERROR_INVALID_NAME:
            return ErrorCode::PathTooLong;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            return ErrorCode::FileNotFound;
        default:
            return fallback;
    }
}

// --- Bridges -----------------------------------------------------------------

ErrorCode ErrorCodeFromExtract(ExtractStatus status, unsigned long lastError) {
    switch (status) {
        case ExtractStatus::Ok:
        case ExtractStatus::Cancelled:
            return ErrorCode::None;
        case ExtractStatus::Untrusted:
            return ErrorCode::Untrusted;
        case ExtractStatus::OpenFailed:
            // Could not open the archive: most often it vanished between launch
            // and read; refine to FileNotFound when the Win32 code says so.
            return RefineWin32WriteError(lastError, ErrorCode::FileNotFound);
        case ExtractStatus::CorruptInput:
            return ErrorCode::Corrupt;
        case ExtractStatus::WriteFailed:
            return RefineWin32WriteError(lastError, ErrorCode::WriteFailed);
        case ExtractStatus::NeedPassword:
            return ErrorCode::NeedPassword;
        case ExtractStatus::Unsupported:
            return ErrorCode::Unsupported;
    }
    return ErrorCode::Internal;
}

ErrorCode ErrorCodeFromPlace(PlaceStatus status, unsigned long lastError) {
    switch (status) {
        case PlaceStatus::Ok:
        case PlaceStatus::NoOutput:
            return ErrorCode::None;
        case PlaceStatus::BadInput:
            return ErrorCode::Internal;
        case PlaceStatus::PlaceFailed:
            return RefineWin32WriteError(lastError, ErrorCode::PlaceFailed);
    }
    return ErrorCode::Internal;
}

ErrorCode ErrorCodeFromPlan(const PipelinePlan& plan) {
    if (!plan.supported() || plan.backend == Backend::None) {
        return ErrorCode::Unsupported;
    }
    return ErrorCode::None;
}

}  // namespace ae
