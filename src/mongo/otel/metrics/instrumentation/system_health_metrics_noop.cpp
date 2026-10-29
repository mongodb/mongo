// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/metrics/instrumentation/system_health_metrics.h"

namespace mongo {

class SystemHealthMetrics::Impl {};

SystemHealthMetrics::SystemHealthMetrics() : _impl(std::make_unique<Impl>()) {}

SystemHealthMetrics::~SystemHealthMetrics() = default;

void SystemHealthMetrics::update(const SystemHealthSnapshot&) {}

void SystemHealthMetrics::recordCollectError() {}

void installSystemHealthOtelMetrics(ServiceContext*) {}

}  // namespace mongo
