/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/util/assert_util.h"

#include <string>

namespace mongo::logv2 {

/** Describes the service (i.e. shard/router) a log line is associated with. */
enum class LogService {
    /**
     * Defer to the thread_local accessed by getLogService() -- do not assign this value to the
     * thread_local
     */
    defer,

    /** Used when we lack context from which to infer a log service (i.e. no thread_local Client) */
    unknown,

    /**
     * Used when the context that we do have is not associated with a log service (i.e., a
     * thread_local Client with ClusterRole::None)
     */
    none,

    /** Shard service */
    shard,

    /** Router service */
    router,
};

/** Accesses the thread-local LogService attribute which log lines reference. */
void setLogService(LogService logService);
LogService getLogService();

/** Returns full name. */
inline StringData toStringData(LogService logService) {
    switch (logService) {
        case LogService::unknown:
            return "unknown";
        case LogService::none:
            return "none";
        case LogService::shard:
            return "shard";
        case LogService::router:
            return "router";
        case LogService::defer:
            MONGO_UNREACHABLE;
    }
    MONGO_UNREACHABLE;
}

/**
 * Returns short name suitable for inclusion in formatted log message (just the first character).
 */
inline StringData getNameForLog(LogService logService) {
    switch (logService) {
        // whenever we don't have a logService, emit "-"
        case LogService::unknown:
        case LogService::none:
            return "-"_sd;
        case LogService::shard:
            return "S"_sd;
        case LogService::router:
            return "R"_sd;
        case LogService::defer:
            MONGO_UNREACHABLE;
    }
    MONGO_UNREACHABLE;
}

/** Appends the full name returned by toStringData(). */
std::ostream& operator<<(std::ostream& os, LogService service);

}  // namespace mongo::logv2
