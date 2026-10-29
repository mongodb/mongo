// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <vector>

[[MONGO_MOD_PUBLIC]];

namespace mongo::timeseries {

/**
 * Determines whether the given 'date' is outside the standard supported range and requires extended
 * range support. Standard range dates can be expressed as a number of seconds since the Unix epoch
 * in 31 unsigned bits.
 */
bool dateOutsideStandardRange(Date_t date);

/**
 * Determines whether any of the given buckets have control.min.timeField values that lie outside
 * the standard range.
 */
boost::optional<Date_t> bucketsHaveDateOutsideStandardRange(
    const TimeseriesOptions& options, std::span<const InsertStatement> inserts);

/**
 * Checks the OID for an extended range time component.
 */
bool oidHasExtendedRangeTime(const OID& oid);

/**
 * Uses a heuristic to determine whether a given time-series collection may contain measurements
 * with dates that fall outside the standard range.
 */
bool collectionMayRequireExtendedRangeSupport(OperationContext* opCtx,
                                              const Collection& collection);

/**
 * Determines whether a time-series collection has an index primarily ordered by a time field. This
 * excludes the clustered index, and is testing specifically if an index's key pattern's first field
 * is either control.min.<timeField> or control.max.<timeField>.
 */
bool collectionHasTimeIndex(OperationContext* opCtx, const Collection& collection);

}  // namespace mongo::timeseries
