// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/ftdc/ftdc_system_stats.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"

#include <string>

namespace mongo {

namespace {

/**
 * Name of FTDC collector to create.
 */
constexpr auto kSystemMetricsCollector = "systemMetrics";

}  // namespace

std::string SystemMetricsCollector::name() const {
    return kSystemMetricsCollector;
}

void SystemMetricsCollector::processStatusErrors(Status s, BSONObjBuilder* builder) {
    if (!s.isOK()) {
        builder->append("error", s.toString());
    }
}

}  // namespace mongo
