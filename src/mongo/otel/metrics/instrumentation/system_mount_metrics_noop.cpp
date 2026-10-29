// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/metrics/instrumentation/system_mount_metrics.h"

namespace mongo {

class SystemMountMetrics::Impl {};

SystemMountMetrics::SystemMountMetrics(std::vector<std::string>)
    : _impl(std::make_unique<Impl>()) {}

SystemMountMetrics::~SystemMountMetrics() = default;

void SystemMountMetrics::update(const BSONObj&) {}

void installSystemMountOtelMetrics(ServiceContext*) {}

}  // namespace mongo
