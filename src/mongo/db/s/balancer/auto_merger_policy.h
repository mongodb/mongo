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

#include "mongo/db/s/balancer/actions_stream_policy.h"
#include "mongo/util/timer.h"

namespace mongo {

class AutoMergerPolicy : public ActionsStreamPolicy {

public:
    AutoMergerPolicy(const std::function<void()>& onStateUpdated)
        : _onStateUpdated(onStateUpdated), _enabled(false), _firstAction(true) {}

    ~AutoMergerPolicy() {}

    /*
     * Enables/disables the AutoMerger.
     */
    void enable();
    void disable();
    bool isEnabled();

    /*
     * Check if the AutoMerger should be reactivated after a period of inactivity.
     */
    void checkInternalUpdates();

    /*
     * ActionsStreamPolicy overridden methods.
     */
    StringData getName() const override;
    boost::optional<BalancerStreamAction> getNextStreamingAction(OperationContext* opCtx) override;
    void applyActionResult(OperationContext* opCtx,
                           const BalancerStreamAction& action,
                           const BalancerStreamActionResponse& result) override;

private:
    void _init(WithLock lk);
    void _checkInternalUpdatesWithLock(WithLock lk);
    std::map<ShardId, std::vector<NamespaceString>> _getNamespacesWithMergeableChunksPerShard(
        OperationContext* opCtx);

private:
    Mutex _mutex = MONGO_MAKE_LATCH("AutoMergerPolicyPolicyImpl::_mutex");

    const std::function<void()> _onStateUpdated;

    bool _enabled;

    bool _firstAction;
    Timer _intervalTimer;
    Timestamp _maxHistoryTimeCurrentRound{0, 0};
    Timestamp _maxHistoryTimePreviousRound{0, 0};
    uint32_t _outstandingActions = 0;

    std::map<ShardId, std::vector<NamespaceString>> _collectionsToMergePerShard;

    friend class AutoMergerPolicyTest;
};
}  // namespace mongo
