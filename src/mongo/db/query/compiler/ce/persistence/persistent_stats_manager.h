// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/util/modules.h"

namespace mongo {

class OperationContext;

/**
 * API between PSAME and the specifics of each persistent statistic, with one implementation per
 * statistic kind. 'StatSpec' describes a statistic of that kind and 'RefreshSummary' is what a
 * refresh reports about the value it persisted.
 */
template <typename StatSpec, typename RefreshSummary>
class [[MONGO_MOD_OPEN]] PersistentStatsManager {
public:
    virtual ~PersistentStatsManager() = default;

    // TODO SERVER-137525: Add load(StatSpec) -> Optional<Stat> for the read path.

    /**
     * Computes the statistic described by 'spec' and persists it, replacing the persisted one.
     */
    virtual StatusWith<RefreshSummary> refresh(OperationContext* opCtx, const StatSpec& spec) = 0;

    // TODO SERVER-135426: Add refreshAllStatsForColl(UUID) -> Status to refresh every statistic
    // persisted for a collection.

    // TODO SERVER-134539: Add deleteAllStatsForColl(UUID) -> Status to delete every statistic
    // persisted for a dropped collection.

    // TODO SERVER-139539: Add ensureOnlyStatsFromColls(std::vector<UUID>) -> Status for the sweep
    // task to delete the statistics of every collection not listed.
};

}  // namespace mongo
