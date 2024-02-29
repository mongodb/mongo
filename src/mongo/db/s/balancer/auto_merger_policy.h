/**
 *    Copyright (C) 2023-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <boost/optional/optional.hpp>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/balancer/actions_stream_policy.h"
#include "mongo/db/s/balancer/balancer_policy.h"
#include "mongo/db/s/balancer/cluster_statistics.h"
#include "mongo/db/shard_id.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/timer.h"

namespace mongo {

/*
 * When the auto-merger is enabled, it works as follows:
 * - Identify all the <shard, collection uuid> pairs for which there are mergeable chunks.
 * - While auto-merge is possible:
 * --- For each shard:
 * ----- For each namespace
 * ------- Schedule a mergeAllChunksOnShard command (max 1000 chunks per time)
 * ----- Apply throttling of `defaultAutoMergerThrottlingMS`
 * - Sleep for `autoMergerIntervalSecs`
 */
class AutoMergerPolicy : public ActionsStreamPolicy {

public:
    AutoMergerPolicy(const std::function<void()>& onStateUpdated)
        : _onStateUpdated(onStateUpdated),
          _enabled(false),
          _firstAction(true),
          _withinRound(true) {}

    ~AutoMergerPolicy() {}

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
    StringData getName() const override;
    boost::optional<BalancerStreamAction> getNextStreamingAction(OperationContext* opCtx) override;
    void applyActionResult(OperationContext* opCtx,
                           const BalancerStreamAction& action,
                           const BalancerStreamActionResponse& result) override;

    /*
     * Maximum number of chunks to merge in one request
     */
    inline static constexpr int MAX_NUMBER_OF_CHUNKS_TO_MERGE = 1000;

private:
    void _init(OperationContext* opCtx, WithLock lk);
    void _checkInternalUpdatesWithLock(OperationContext* opCtx, WithLock lk);
    std::map<ShardId, std::vector<NamespaceString>> _getNamespacesWithMergeableChunksPerShard(
        OperationContext* opCtx);

private:
    Mutex _mutex = MONGO_MAKE_LATCH("AutoMergerPolicyPolicyImpl::_mutex");

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
