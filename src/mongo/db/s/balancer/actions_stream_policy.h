// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/s/balancer/balancer_policy.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {

class ActionsStreamPolicy {
public:
    virtual ~ActionsStreamPolicy() {}

    virtual std::string_view getName() const = 0;

    /**
     * Generates a descriptor detailing the next streaming action (and the targeted
     * collection/chunk[s]) to be performed.
     *
     * The balancer is expected to execute a command matching the content of the descriptor and to
     * invoke the related acknowledge() method on the defragmentation policy once the result is
     * available (this will allow to update the progress of the algorithm).
     */
    virtual boost::optional<BalancerStreamAction> getNextStreamingAction(
        OperationContext* opCtx) = 0;

    /**
     * Updates the internal status of the policy by notifying the result of an action previously
     * retrieved through getNextStreamingAction().
     * The types of action and response are expected to match - or an std::bad_variant_access
     * error will be thrown.
     */
    virtual void applyActionResult(OperationContext* opCtx,
                                   const BalancerStreamAction& action,
                                   const BalancerStreamActionResponse& result) = 0;
};

}  // namespace mongo
