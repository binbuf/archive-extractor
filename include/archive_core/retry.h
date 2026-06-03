#pragma once

#include <cstdint>
#include <functional>

namespace ae {

// --- Retry-with-backoff for transiently-locked filesystem ops ----------------
//
// Antivirus / search-indexer / another process can briefly hold a just-written
// destination file open, making MoveFileEx / delete fail with a transient
// sharing violation (design 06 "File in use / locked on move"; 07 §14 AV lock).
// These ops usually succeed on a short retry. This helper runs an operation up
// to `attempts` times with a brief exponential backoff between tries, retrying
// ONLY on transient/lock-style Win32 errors.

// Tunable retry policy. Defaults: 5 attempts, 20 ms initial backoff doubling up
// to 200 ms — a few hundred ms total worst case, imperceptible to the user but
// enough to clear a typical AV scan window.
struct RetryPolicy {
    int attempts = 5;
    unsigned initialDelayMs = 20;
    unsigned maxDelayMs = 200;
};

// True when `win32Error` (a GetLastError value) is the kind of transient lock /
// sharing failure worth retrying. Permanent errors (access denied, disk full,
// not found, ...) are NOT retried — they fail immediately.
bool IsTransientLockError(unsigned long win32Error);

// Run `op` until it returns true, or until the attempt budget is exhausted.
// `op` must return true on success; on failure it returns false AND its raw
// GetLastError value via the out-param so the helper can decide whether to
// retry. Retries only while the last error is transient (IsTransientLockError);
// a permanent error stops immediately. Returns true if any attempt succeeded.
//
// `sleepMs` is injectable so unit tests can run with a no-op sleep (no real
// wall-clock delay); production callers use the default (real ::Sleep).
bool RetryWithBackoff(const std::function<bool(unsigned long&)>& op,
                      const RetryPolicy& policy = {},
                      const std::function<void(unsigned)>& sleepMs = nullptr);

}  // namespace ae
