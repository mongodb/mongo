// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/compiler/stats/collection_statistics.h"
#include "mongo/db/query/compiler/stats/stats_cache_loader.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

#include <thread>


namespace mongo::stats {

class StatsCacheLoaderMock : public StatsCacheLoader {
public:
    SemiFuture<StatsCacheVal> getStats(OperationContext* opCtx,
                                       const StatsPathString& statsPath) override;

    void setStatsReturnValueForTest(StatusWith<StatsCacheVal> swStats) override;

    static const Status kInternalErrorStatus;

private:
    StatusWith<StatsCacheVal> _swStatsReturnValueForTest{kInternalErrorStatus};
};

}  // namespace mongo::stats
