#include "archive_core/retry.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace ae {

bool IsTransientLockError(unsigned long e) {
    switch (e) {
        case ERROR_SHARING_VIOLATION:  // file open by another process
        case ERROR_LOCK_VIOLATION:     // a region is locked
        case ERROR_ACCESS_DENIED:      // AV/indexer often surfaces a lock this way
        case ERROR_USER_MAPPED_FILE:   // a mapped view still pins the file
            return true;
        default:
            return false;
    }
}

bool RetryWithBackoff(const std::function<bool(unsigned long&)>& op,
                      const RetryPolicy& policy,
                      const std::function<void(unsigned)>& sleepMs) {
    const int attempts = policy.attempts < 1 ? 1 : policy.attempts;
    unsigned delay = policy.initialDelayMs;

    for (int i = 0; i < attempts; ++i) {
        unsigned long err = 0;
        if (op(err)) return true;

        const bool lastTry = (i + 1 >= attempts);
        if (lastTry || !IsTransientLockError(err)) {
            // Permanent error, or out of attempts: stop now (no point waiting).
            return false;
        }

        if (sleepMs) {
            sleepMs(delay);
        } else {
            ::Sleep(delay);
        }
        delay *= 2;
        if (delay > policy.maxDelayMs) delay = policy.maxDelayMs;
    }
    return false;
}

}  // namespace ae
