// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

/**
 * Mongo exit codes.
 */

namespace [[MONGO_MOD_PUBLIC]] mongo {

enum class ExitCode {
    clean = 0,
    fail = 1,
    badOptions = 2,
    replicationError = 3,
    needUpgrade = 4,
    shardingError = 5,
    kill = 12,
    abrupt = 14,
#ifdef _WIN32
    ntServiceError = 20,
#endif  // _WIN32
    java [[deprecated]] = 21,
    oomMalloc [[deprecated]] = 42,
    oomRealloc [[deprecated]] = 43,
    fs [[deprecated]] = 45,
    clockSkew [[deprecated]] = 47,  // OpTime
    netError = 48,
#ifdef _WIN32
    windowsServiceStop [[deprecated]] = 49,
#endif  // _WIN32
    launcherMiddleError = 50,
    launcherError = 51,
    possibleCorruption [[deprecated]] = 60,  // e.g. buffer overflow
    watchdog = 61,                           // Internal Watchdog has terminated mongod
    needDowngrade = 62,                      // This exe can't use the existing data files
    reservedBegin = 64,       // FreeBSD uses this range. Avoiding to prevent confusion.
    reservedEnd = 78,         // FreeBSD uses this range. Avoiding to prevent confusion.
    threadSanitizer = 86,     // Default exit code for Thread Sanitizer failures
    processHealthCheck = 87,  // Process health check triggered the crash.
    uncaught = 100,           // top level exception that wasn't caught
    test [[deprecated]] = 101,
    auditRotateError = 102  // The startup rotation of audit logs failed
};

}  // namespace mongo
