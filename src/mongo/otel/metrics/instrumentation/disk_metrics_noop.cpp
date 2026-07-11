// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/metrics/instrumentation/disk_metrics.h"

namespace mongo {

class DiskMetrics::Impl {};

DiskMetrics::DiskMetrics(std::vector<std::string>) : _impl(std::make_unique<Impl>()) {}

DiskMetrics::~DiskMetrics() = default;

void DiskMetrics::update(BSONObj) {}

void installDiskOtelMetrics(ServiceContext*) {}

}  // namespace mongo
