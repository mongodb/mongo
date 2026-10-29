// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/yield_policy_callbacks_impl.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/curop.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/time_support.h"

#include <string>
#include <utility>

namespace mongo {
namespace {
MONGO_FAIL_POINT_DEFINE(setInterruptOnlyPlansCheckForInterruptHang);
MONGO_FAIL_POINT_DEFINE(setYieldAllLocksHang);
MONGO_FAIL_POINT_DEFINE(setYieldAllLocksHangSecond);
MONGO_FAIL_POINT_DEFINE(setYieldAllLocksWait);
}  // namespace

YieldPolicyCallbacksImpl::YieldPolicyCallbacksImpl(NamespaceString nssForFailpoints)
    : _nss(std::move(nssForFailpoints)) {}

void YieldPolicyCallbacksImpl::duringYield(OperationContext* opCtx) const {
    CurOp::get(opCtx)->yielded();

    _tryLogLongRunningQueries(opCtx);

    // If we yielded because we encountered the need to refresh the sharding CatalogCache, refresh
    // it here while the locks are yielded.
    auto& catalogCacheRefreshRequired =
        planExecutorShardingState(opCtx).catalogCacheRefreshRequired;
    if (catalogCacheRefreshRequired) {
        // We are simply joining the refresh of the routing tables for the affected namespace here
        // but not performing a routing operation, so this routing table access is not gated behind
        // a RoutingContext.
        auto catalogCache = Grid::get(opCtx)->catalogCache();
        uassertStatusOK(
            catalogCache->getCollectionRoutingInfo(opCtx, *catalogCacheRefreshRequired));
        catalogCacheRefreshRequired.reset();
    }

    const auto& nss = _nss;
    auto failPointHang = [opCtx, nss](FailPoint* fp) {
        fp->executeIf(
            [opCtx, fp](const BSONObj& config) {
                fp->pauseWhileSet();

                if (config.getField("checkForInterruptAfterHang").trueValue()) {
                    // Throws.
                    opCtx->checkForInterrupt();
                }
            },
            [nss](const BSONObj& config) {
                const auto fpNss = NamespaceStringUtil::parseFailPointData(config, "namespace");
                return fpNss.isEmpty() || fpNss == nss;
            });
    };
    failPointHang(&setYieldAllLocksHang);
    failPointHang(&setYieldAllLocksHangSecond);

    setYieldAllLocksWait.executeIf(
        [&](const BSONObj& data) { sleepFor(Milliseconds(data["waitForMillis"].numberInt())); },
        [&](const BSONObj& config) {
            const auto fpNss = NamespaceStringUtil::parseFailPointData(config, "namespace");
            return fpNss.isEmpty() || _nss == fpNss;
        });
}

void YieldPolicyCallbacksImpl::preCheckInterruptOnly(OperationContext* opCtx) const {
    _tryLogLongRunningQueries(opCtx);

    // If the 'setInterruptOnlyPlansCheckForInterruptHang' fail point is enabled, set the
    // 'failPointMsg' field of this operation's CurOp to signal that we've hit this point.
    if (MONGO_unlikely(setInterruptOnlyPlansCheckForInterruptHang.shouldFail())) {
        CurOpFailpointHelpers::waitWhileFailPointEnabled(
            &setInterruptOnlyPlansCheckForInterruptHang,
            opCtx,
            "setInterruptOnlyPlansCheckForInterruptHang");
    }
}

void YieldPolicyCallbacksImpl::_tryLogLongRunningQueries(OperationContext* opCtx) const {
    CurOp::get(opCtx)->maybeLogSlowQuery();
}

}  // namespace mongo
