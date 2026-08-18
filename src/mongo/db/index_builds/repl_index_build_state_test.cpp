// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index_builds/repl_index_build_state.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/client.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/version_context.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

// (Generic FCV reference): used for testing, should exist across LTS binary versions
static const VersionContext kLatestVersionContext{multiversion::GenericFCV::kLatest};

ReplIndexBuildState makeReplState() {
    return ReplIndexBuildState(UUID::gen(),
                               UUID::gen(),
                               DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                               std::vector<IndexBuildInfo>{},
                               IndexBuildProtocol::kTwoPhase,
                               Date_t::now());
}

class ReplIndexBuildStateTest : public ServiceContextTest {
public:
    ServiceContext::UniqueOperationContext opCtxHolder{makeOperationContext()};
    OperationContext* opCtx{opCtxHolder.get()};
};

TEST_F(ReplIndexBuildStateTest, GetOutcomeFutureNotReadyInitially) {
    auto replState = makeReplState();
    ASSERT_FALSE(replState.getOutcomeFuture().isReady());
}

TEST_F(ReplIndexBuildStateTest, FulfillOutcomeSuccess) {
    auto replState = makeReplState();

    ReplIndexBuildState::IndexCatalogStats stats;
    stats.numIndexesBefore = 2;
    stats.numIndexesAfter = 4;
    replState.fulfillOutcome(opCtx, stats);

    ASSERT_TRUE(replState.getOutcomeFuture().isReady());
    auto result = unittest::assertGet(replState.getOutcomeFuture().getNoThrow());
    EXPECT_EQ(2, result.numIndexesBefore);
    EXPECT_EQ(4, result.numIndexesAfter);
}

TEST_F(ReplIndexBuildStateTest, FulfillOutcomeError) {
    auto replState = makeReplState();

    replState.fulfillOutcome(opCtx, Status{ErrorCodes::IndexBuildAborted, "aborted"});

    ASSERT_TRUE(replState.getOutcomeFuture().isReady());
    auto result = replState.getOutcomeFuture().getNoThrow();
    ASSERT_FALSE(result.isOK());
    EXPECT_EQ(ErrorCodes::IndexBuildAborted, result.getStatus().code());
}

TEST_F(ReplIndexBuildStateTest, FulfillOutcomeClearsLongRunningMarker) {
    auto replState = makeReplState();

    {
        ClientLock lk(opCtx->getClient());
        VersionContext::setDecoration(lk, opCtx, kLatestVersionContext);
        VersionContext::markDecorationAsLongRunning(lk, opCtx);
    }
    ASSERT_TRUE(VersionContext::getDecoration(opCtx).isLongRunningOperation());

    replState.fulfillOutcome(opCtx, ReplIndexBuildState::IndexCatalogStats{});

    ASSERT_FALSE(VersionContext::getDecoration(opCtx).isLongRunningOperation());
    ASSERT_TRUE(VersionContext::getDecoration(opCtx).hasOperationFCV());
    ASSERT_EQ(kLatestVersionContext, VersionContext::getDecoration(opCtx));
}

TEST_F(ReplIndexBuildStateTest, FulfillOutcomeWithoutLongRunningMarker) {
    auto replState = makeReplState();

    VersionContext::FixedOperationFCVRegion fixedOfcvRegion(opCtx);
    ASSERT_FALSE(VersionContext::getDecoration(opCtx).isLongRunningOperation());

    ReplIndexBuildState::IndexCatalogStats stats;
    stats.numIndexesBefore = 1;
    stats.numIndexesAfter = 2;
    replState.fulfillOutcome(opCtx, stats);

    ASSERT_FALSE(VersionContext::getDecoration(opCtx).isLongRunningOperation());
    auto result = unittest::assertGet(replState.getOutcomeFuture().getNoThrow());
    EXPECT_EQ(1, result.numIndexesBefore);
    EXPECT_EQ(2, result.numIndexesAfter);
}

}  // namespace
}  // namespace mongo
