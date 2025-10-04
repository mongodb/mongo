/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/s/balancer/balancer_policy.h"

namespace mongo {

class ActionsStreamPolicy {
public:
    virtual ~ActionsStreamPolicy() {}

    virtual StringData getName() const = 0;

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
