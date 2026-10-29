// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/util/modules.h"

#include <string_view>

[[MONGO_MOD_PUBLIC]];
namespace mongo::timeseries {
BSONObj generateTimeseriesValidator(int bucketVersion, std::string_view timeField);

/**
 * Validates that 'validator' matches one of the schemas that the server generates for the supported
 * time-series bucket format versions on 'timeField'. Throws a user-facing error when no known
 * version matches.
 */
void validateTimeseriesValidator(const BSONObj& validator, std::string_view timeField);

/**
 * Generates the index on the metaField and timeField for time-series collections. This is used for
 * query-based reopening.
 */
Status createDefaultTimeseriesIndex(OperationContext* opCtx,
                                    CollectionWriter& collection,
                                    BSONObj collator);
}  // namespace mongo::timeseries
