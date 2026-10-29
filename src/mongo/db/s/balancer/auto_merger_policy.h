// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/balancer/actions_stream_policy.h"
#include "mongo/db/s/balancer/balancer_policy.h"
#include "mongo/db/s/balancer/cluster_statistics.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/modules.h"
#include "mongo/util/timer.h"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string_view>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

/*
 * When the auto-merger is enabled, it works as follows:
 * - Identify all the <shard, collection uuid> pairs for which there are mergeable chunks.
 * - While auto-merge is possible:
 * -- For each shard:
 * --- For each namespace
 * ----- Schedule a mergeAllChunksOnShard command (max `autoMergerMaxChunksToMerge` chunks per time)
 * --- Apply throttling of `autoMergerThrottlingMS`
 * - Sleep for `autoMergerIntervalSecs`
 */
class AutoMergerPolicy : public ActionsStreamPolicy {

public:
    AutoMergerPolicy(const std::function<void()>& onStateUpdated)
        : _onStateUpdated(onStateUpdated),
          _enabled(false),
          _firstAction(true),
          _withinRound(true) {}

    ~AutoMergerPolicy() override {}

    /*
     * Enables/disables the AutoMerger.
     */
    void enable(OperationContext* opCtx);
    void disable(OperationContext* opCtx);
    bool isEnabled();

    /*
     * Check if the AutoMerger should be reactivated after a period of inactivity.
     */
    void checkInternalUpdates(OperationContext* opCtx);

    /*
     * ActionsStreamPolicy overridden methods.
     */
    std::string_view getName() const override;
    boost::optional<BalancerStreamAction> getNextStreamingAction(OperationContext* opCtx) override;
    void applyActionResult(OperationContext* opCtx,
                           const BalancerStreamAction& action,
                           const BalancerStreamActionResponse& result) override;

private:
    void _init(OperationContext* opCtx, WithLock lk);
    void _checkInternalUpdatesWithLock(OperationContext* opCtx, WithLock lk);
    std::map<ShardId, std::vector<NamespaceString>> _getNamespacesWithMergeableChunksPerShard(
        OperationContext* opCtx);

private:
    std::mutex _mutex;

    inline static constexpr int MAX_NUMBER_OF_CONCURRENT_MERGE_ACTIONS = 10;

    const std::function<void()> _onStateUpdated;

    bool _enabled;

    bool _firstAction;
    // Set if there could be more mergeable chunks to process.
    bool _withinRound;
    Timer _intervalTimer;
    Timestamp _maxHistoryTimeCurrentRound{0, 0};
    Timestamp _maxHistoryTimePreviousRound{0, 0};
    uint32_t _outstandingActions = 0;

    // Map initially populated by querying `config.chunks` and - during an auto-merge window -
    // potentially repopulated with the content of _rescheduledCollectionsToMergePerShard.
    std::map<ShardId, std::vector<NamespaceString>> _collectionsToMergePerShard;

    // When a merge succeeds and some chunks were merged, the action gets rescheduled.
    // When a merge succeeds with no merged chunks, the action does not get rescheduled.
    std::map<ShardId, std::vector<NamespaceString>> _rescheduledCollectionsToMergePerShard;

    friend class AutoMergerPolicyTest;
};
}  // namespace mongo
