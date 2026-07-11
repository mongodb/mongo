// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>

namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] logv2 {
using namespace std::literals::string_view_literals;

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
inline std::string_view toStringData(LogService logService) {
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
inline std::string_view getNameForLog(LogService logService) {
    switch (logService) {
        // whenever we don't have a logService, emit "-"
        case LogService::unknown:
        case LogService::none:
            return "-"sv;
        case LogService::shard:
            return "S"sv;
        case LogService::router:
            return "R"sv;
        case LogService::defer:
            MONGO_UNREACHABLE;
    }
    MONGO_UNREACHABLE;
}

/** Appends the full name returned by toStringData(). */
std::ostream& operator<<(std::ostream& os, LogService service);

}  // namespace logv2
}  // namespace mongo
