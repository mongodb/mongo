// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"

[[MONGO_MOD_PUBLIC]];

namespace mongo::timeseries {

/**
 * Cluster-level upgrade/downgrade between viewful and viewless timeseries collections.
 *
 * For upgrade (isUpgrade=true): Converts legacy viewful timeseries collections to viewless format.
 * For downgrade (isUpgrade=false): Converts viewless timeseries collections to viewful format.
 *
 * This function must be called on the config server.
 *
 * TODO (SERVER-116499): Remove this once 9.0 becomes last LTS.
 */
void upgradeDowngradeViewlessTimeseriesInShardedCluster(OperationContext* opCtx, bool isUpgrade);

}  // namespace mongo::timeseries
