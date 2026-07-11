// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/compiler/stats/ce_histogram.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>


namespace mongo::stats {
using namespace std::literals::string_view_literals;
using StatsPathString = std::pair<NamespaceString, std::string>;
using StatsCacheVal = std::shared_ptr<const CEHistogram>;

class StatsCacheLoader {
public:
    /**
     * Non-blocking call, which returns CollectionStatistics from the the persistent metadata store.
     *
     * If for some reason the asynchronous fetch operation cannot be dispatched (for example on
     * shutdown), throws a DBException.
     */
    virtual SemiFuture<StatsCacheVal> getStats(OperationContext* opCtx,
                                               const StatsPathString& statsPath) = 0;

    virtual void setStatsReturnValueForTest(StatusWith<StatsCacheVal> swStats) {};

    virtual ~StatsCacheLoader() {}

    static constexpr std::string_view kStatsPrefix = "system.statistics"sv;
};

}  // namespace mongo::stats
