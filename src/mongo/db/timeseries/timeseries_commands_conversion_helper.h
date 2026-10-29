// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/optional/optional.hpp>

[[MONGO_MOD_PUBLIC]];

namespace mongo::timeseries {
using namespace std::literals::string_view_literals;

// TODO SERVER-101896 remove this constant once 9.0 becomes last LTS and all timeseries are viewless
inline constexpr std::string_view kIsTimeseriesNamespaceFieldName = "isTimeseriesNamespace"sv;

/**
 * Returns a command object with time-series view namespace translated to bucket namespace.
 *
 * TODO SERVER-101896 remove this function once 9.0 becomes last LTS and all tiemseries are viewless
 */
BSONObj makeTimeseriesCommand(const BSONObj& origCmd,
                              const NamespaceString& ns,
                              std::string_view nsFieldName,
                              boost::optional<std::string_view> appendTimeSeriesFlag);

mongo::BSONObj translateIndexSpecFromLogicalToBuckets(OperationContext* opCtx,
                                                      const NamespaceString& origNs,
                                                      const BSONObj& origIndex,
                                                      const TimeseriesOptions& options);
}  // namespace mongo::timeseries
