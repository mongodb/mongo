// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/aggregation_context_fixture.h"

#include "mongo/db/curop.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/shard_role/lock_manager/locker.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/logv2/log_component.h"
#include "mongo/unittest/unittest.h"

#include <boost/optional.hpp>

namespace mongo {
namespace {

void runCurOpLifecycleWithSlowOpLogging(OperationContext* opCtx) {
    auto curOp = CurOp::get(*opCtx);
    curOp->ensureStarted();
    curOp->done();
    curOp->completeAndLogOperation({logv2::LogComponent::kTest},
                                   nullptr /* filter */,
                                   boost::none /* responseLength */,
                                   0 /* slowMsOverride */,
                                   true /* forceLog */);
}

TEST_F(AggregationContextFixture, CurOpLifecycleOnFixtureOpCtx) {
    auto* opCtx = getExpCtx()->getOperationContext();
    ASSERT(shard_role_details::getLocker(opCtx));
    runCurOpLifecycleWithSlowOpLogging(opCtx);
}

// Covers the direct client->makeOperationContext() pattern used by benchmarks.
TEST_F(AggregationContextFixture, CurOpLifecycleOnDirectClientOpCtx) {
    auto client = getServiceContext()->getService()->makeClient("direct-client");
    auto opCtx = client->makeOperationContext();
    ASSERT(shard_role_details::getLocker(opCtx.get()));
    runCurOpLifecycleWithSlowOpLogging(opCtx.get());
}

}  // namespace
}  // namespace mongo
