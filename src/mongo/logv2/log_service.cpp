// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/logv2/log_service.h"

#include "mongo/util/assert_util.h"

#include <string>

namespace mongo::logv2 {

namespace {

thread_local LogService logService = LogService::unknown;

}  // namespace

void setLogService(LogService service) {
    invariant(service != LogService::defer);
    logService = std::move(service);
}

LogService getLogService() {
    return logService;
}

std::ostream& operator<<(std::ostream& os, LogService service) {
    return os << toStringData(service);
}

}  // namespace mongo::logv2
