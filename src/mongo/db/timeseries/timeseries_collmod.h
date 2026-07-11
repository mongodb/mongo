// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/ddl/coll_mod_gen.h"
#include "mongo/util/modules.h"

#include <memory>

[[MONGO_MOD_PUBLIC]];

namespace mongo::timeseries {

/**
 * Returns a CollMod on the underlying buckets collection of the time-series collection.
 *
 * TODO SERVER-105548 remove this function once 9.0 becomes last LTS
 */
std::unique_ptr<CollMod> makeTimeseriesBucketsCollModCommand(TimeseriesOptions& timeseriesOptions,
                                                             const CollMod& origCmd,
                                                             bool isLegacyTimeseries = true);

/**
 * Returns a CollMod on the view definition of the time-series collection. Returns null if the view
 * definition need not be changed or if the modifications are invalid.
 *
 * TODO SERVER-105548 remove this function once 9.0 becomes last LTS
 */
std::unique_ptr<CollMod> makeTimeseriesViewCollModCommand(TimeseriesOptions& timeseriesOptions,
                                                          const CollMod& origCmd);

/**
 * Performs the collection modification described in "cmd" on the collection "nss". May perform
 * timeseries view translation to multiple collMod if "performViewChange" flag is set.
 *
 * TODO SERVER-105548 remove this function once 9.0 becomes last LTS
 */
Status processCollModCommandWithTimeSeriesTranslation(OperationContext* opCtx,
                                                      const NamespaceString& nss,
                                                      const CollMod& cmd,
                                                      bool performViewChange,
                                                      BSONObjBuilder* result);

}  // namespace mongo::timeseries
