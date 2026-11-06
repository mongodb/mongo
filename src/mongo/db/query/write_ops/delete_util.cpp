// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/write_ops/delete_util.h"

#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/plan_yield_policy.h"

namespace mongo {
namespace delete_util {

void makeDeleteRequest(OperationContext* opCtx,
                       const write_ops::FindAndModifyCommandRequest& request,
                       bool explain,
                       DeleteRequest* requestOut) {
    requestOut->setQuery(request.getQuery());
    requestOut->setProj(request.getFields().value_or(BSONObj()));
    requestOut->setLegacyRuntimeConstants(
        request.getLegacyRuntimeConstants().value_or(Variables::generateRuntimeConstants(opCtx)));
    requestOut->setLet(request.getLet());
    requestOut->setSort(request.getSort().value_or(BSONObj()));
    requestOut->setHint(request.getHint());
    requestOut->setCollation(request.getCollation().value_or(BSONObj()));
    requestOut->setMulti(false);
    requestOut->setReturnDeleted(true);  // Always return the old value.
    requestOut->setIsExplain(explain);

    requestOut->setYieldPolicy(PlanYieldPolicy::YieldPolicy::YIELD_AUTO);
    requestOut->setIsTimeseriesNamespace(request.getIsTimeseriesNamespace());
}

}  // namespace delete_util
}  // namespace mongo
