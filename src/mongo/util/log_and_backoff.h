// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_detail.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <cstdint>

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace log_backoff_detail {
void logAndBackoffImpl(size_t numAttempts);
}  // namespace log_backoff_detail

/**
 * Will log a message at 'logLevel' for the given 'logComponent' and will perform an increasing
 * backoff, with the backoff period based on 'numAttempts'.
 */
template <size_t N, typename... Args>
void logAndBackoff(int32_t logId,
                   logv2::LogComponent component,
                   logv2::LogSeverity severity,
                   size_t numAttempts,
                   const char (&msg)[N],
                   const logv2::detail::NamedArg<Args>&... args) {
    logv2::detail::doLog(logId, severity, {component}, msg, args..., "attempts"_attr = numAttempts);
    log_backoff_detail::logAndBackoffImpl(numAttempts);
}

}  // namespace mongo
