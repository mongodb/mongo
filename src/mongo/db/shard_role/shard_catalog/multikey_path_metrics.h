// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <cstdint>

namespace mongo {
class BSONObjBuilder;
class OperationContext;

namespace catalog_metrics {

[[MONGO_MOD_PUBLIC]]
void appendMultikeyPathStatsToIndexStats(BSONObjBuilder* indexStatsBuilder);

void recordOrdinaryMultikeyPathChanges(OperationContext* opCtx, int64_t count);

void recordWildcardMultikeyPathChanges(OperationContext* opCtx, int64_t count);

void recordSideTransaction();

}  // namespace catalog_metrics
}  // namespace mongo
