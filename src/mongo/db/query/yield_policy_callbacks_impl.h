// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/util/modules.h"

namespace mongo {

class YieldPolicyCallbacksImpl final : public YieldPolicyCallbacks {
public:
    /**
     * Although yielding is not dependent on a particular collection name, there are failpoints
     * which should cause hangs only for queries over a particular collection. 'nssForFailpoints'
     * names the collection for which yielding-related failpoints should be enabled.
     */
    explicit YieldPolicyCallbacksImpl(NamespaceString nssForFailpoints);

    ~YieldPolicyCallbacksImpl() override = default;

    void duringYield(OperationContext*) const override;
    void preCheckInterruptOnly(OperationContext*) const override;

private:
    void _tryLogLongRunningQueries(OperationContext*) const;

    NamespaceString _nss;
};

}  // namespace mongo
