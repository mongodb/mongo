// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/s/balancer/actions_stream_policy.h"
#include "mongo/db/s/balancer/balancer_policy.h"
#include "mongo/util/modules.h"

#include <functional>
#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo {

class MoveUnshardedPolicy : public ActionsStreamPolicy {
public:
    MoveUnshardedPolicy();

    std::string_view getName() const override {
        static std::string_view name("MoveUnshardedPolicy");
        return name;
    };

    boost::optional<BalancerStreamAction> getNextStreamingAction(OperationContext* opCtx) override {
        return boost::none;
    }

    void applyActionResult(OperationContext* opCtx,
                           const BalancerStreamAction& action,
                           const BalancerStreamActionResponse& response) override;

    MigrateInfoVector selectCollectionsToMove(
        OperationContext* opCtx,
        const std::vector<ClusterStatistics::ShardStatistics>& allShards,
        stdx::unordered_set<ShardId>* availableShards,
        bool onlyTrackedCollection = false);

private:
    FailPoint* fpBalancerShouldReturnRandomMigrations;
};

}  // namespace mongo
