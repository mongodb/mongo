// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding/resharding_oplog_applier_progress_gen.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"

#include <cstdint>

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Not thread safe and should only be called on a single threaded context.
 */
class ReshardingOplogApplierMetrics {
public:
    ReshardingOplogApplierMetrics(ShardId donorShardId,
                                  ReshardingMetrics* metrics,
                                  boost::optional<ReshardingOplogApplierProgress> progressDoc);

    void onInsertApplied();
    void onUpdateApplied();
    void onDeleteApplied();

    void updateAverageTimeToApplyOplogEntries(Milliseconds timeToApply);

    void onBatchRetrievedDuringOplogApplying(Milliseconds elapsed);
    void onOplogLocalBatchApplied(Milliseconds elapsed);
    void onOplogEntriesApplied(int64_t numEntries);
    void onWriteToStashCollections();

    int64_t getInsertsApplied() const;
    int64_t getUpdatesApplied() const;
    int64_t getDeletesApplied() const;
    int64_t getOplogEntriesApplied() const;
    int64_t getWritesToStashCollections() const;

private:
    const ShardId _donorShardId;

    ReshardingMetrics* _metrics;
    Atomic<int64_t> _insertsApplied{0};
    Atomic<int64_t> _updatesApplied{0};
    Atomic<int64_t> _deletesApplied{0};
    Atomic<int64_t> _oplogEntriesApplied{0};
    Atomic<int64_t> _writesToStashCollections{0};
};

}  // namespace mongo
