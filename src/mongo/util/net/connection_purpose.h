// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/otel/metrics/metrics_attributes.h"
#include "mongo/transport/session.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

const otel::metrics::AttributeDefinition<int64_t>& replicationIdAttribute();

/** Gets replication ID for OTel metrics purposes */
boost::optional<int64_t>& getReplicationId(transport::Session* session);

/**
 * Specifies the purpose of a connection. Currently used to collect  unique metrics on
 * replication connections.
 */
enum class ConnectionPurpose { kDefault = 0, kReplication = 1 };
ConnectionPurpose& getConnectionPurpose(transport::Session* session);
}  // namespace mongo
