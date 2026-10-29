// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/metrics/instrumentation/process_health_metrics.h"

namespace mongo {

class ProcessHealthMetrics::Impl {};

ProcessHealthMetrics::ProcessHealthMetrics() : _impl(std::make_unique<Impl>()) {}
ProcessHealthMetrics::~ProcessHealthMetrics() = default;

void ProcessHealthMetrics::update(const ProcessHealthSnapshot&) {}
void ProcessHealthMetrics::recordCollectError() {}

void installProcessHealthOtelMetrics(ServiceContext*) {}

}  // namespace mongo
