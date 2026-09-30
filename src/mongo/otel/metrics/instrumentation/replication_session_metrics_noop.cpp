// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/metrics/instrumentation/replication_session_metrics.h"

namespace mongo {

class SessionMetrics::Impl {};

SessionMetrics::SessionMetrics() : _impl(std::make_unique<Impl>()) {}

SessionMetrics::~SessionMetrics() = default;

void SessionMetrics::update(const std::vector<transport::SessionStats>&) {}
void SessionMetrics::recordCollectError() {}

void installReplicationSessionOtelMetrics(ServiceContext*) {}
}  // namespace mongo
