// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/query/compiler/stats/stats_cache_loader.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

namespace mongo::stats {

class [[MONGO_MOD_PUBLIC]] StatsCacheLoaderImpl : public StatsCacheLoader {
public:
    SemiFuture<StatsCacheVal> getStats(OperationContext* opCtx,
                                       const StatsPathString& statsPath) override;
};

}  // namespace mongo::stats
