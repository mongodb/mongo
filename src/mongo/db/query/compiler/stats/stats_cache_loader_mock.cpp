// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/stats/stats_cache_loader_mock.h"

#include "mongo/base/error_codes.h"

#include <utility>

namespace mongo::stats {

const Status StatsCacheLoaderMock::kInternalErrorStatus = {
    ErrorCodes::InternalError, "Stats cache loader received unexpected request"};

SemiFuture<StatsCacheVal> StatsCacheLoaderMock::getStats(OperationContext* opCtx,
                                                         const StatsPathString& statsPath) {

    return makeReadyFutureWith([this] { return _swStatsReturnValueForTest; }).semi();
}

void StatsCacheLoaderMock::setStatsReturnValueForTest(StatusWith<StatsCacheVal> swStats) {
    _swStatsReturnValueForTest = std::move(swStats);
}
}  // namespace mongo::stats
