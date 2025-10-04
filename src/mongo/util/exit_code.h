/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

/**
 * Mongo exit codes.
 */

namespace mongo {

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
