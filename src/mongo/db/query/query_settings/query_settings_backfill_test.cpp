/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/query/query_settings/query_settings_backfill.h"

#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/query_settings/query_settings_manager.h"
#include "mongo/db/sharding_environment/sharding_mongos_test_fixture.h"
#include "mongo/idl/server_parameter_test_controller.h"

#include <memory>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::query_settings {
namespace {

using query_shape::QueryShapeHash;

/**
 * Creates a mock QueryShapeHash and QueryInstance pair for testing purposes by creating a BSONObj
 * with a single field named 'fieldName', and then hashing it for the 'queryShapeHash'.
 */
std::pair<QueryShapeHash, QueryInstance> makeMockQueryShapeHashAndInstance(StringData fieldName) {
    auto queryInstance = BSON(fieldName << 0);
    auto queryShapeHash =
        SHA256Block::computeHash((const uint8_t*)queryInstance.objdata(), queryInstance.objsize());
    return std::make_pair(std::move(queryShapeHash), std::move(queryInstance));
}

/**
 * Return data class used to propagate the results received via "OnCompletionHook" arguments.
 * Returned by "BackfillCoordinatorTest::expectFutureBackfillCompletion()'.
 */
struct BackfillOnCompletionHookResponse {
    std::vector<QueryShapeHash> hashes;
    LogicalTime time;
    boost::optional<TenantId> tenantId;
};

/**
 * A "BackfillCoordinator" abstract class implmentation which allows mocking the
 * 'insertRepresentativeQueriesToCollection()' behaviour and inspecting the internal state for ease
 * of testing.
 */
class BackfillCoordinatorForTest : public BackfillCoordinator {
public:
    using BackfillCoordinator::State;
    using MockInsertRepresentativeQueriesFn =
        std::function<std::vector<QueryShapeHash>(std::vector<QueryShapeRepresentativeQuery>)>;

    BackfillCoordinatorForTest(BackfillCoordinator::OnCompletionHook hook,
                               std::shared_ptr<executor::TaskExecutor> executor)
        : query_settings::BackfillCoordinator(std::move(hook)), _executor(std::move(executor)) {}

    State* peekState() {
        stdx::lock_guard lk(_mutex);
        return _state.get();
    }

    MockInsertRepresentativeQueriesFn mockInsertRepresentativeQueriesFn;

private:
    ExecutorFuture<std::vector<query_shape::QueryShapeHash>>
    insertRepresentativeQueriesToCollection(
        OperationContext* opCtx,
        std::vector<QueryShapeRepresentativeQuery> representativeQueries,
        std::shared_ptr<executor::TaskExecutor> executor) final {
        return ExecutorFuture<std::vector<query_shape::QueryShapeHash>>(
            executor, mockInsertRepresentativeQueriesFn(std::move(representativeQueries)));
    }

    std::shared_ptr<executor::TaskExecutor> makeExecutor(OperationContext* opCtx) final {
        return _executor;
    }

    std::unique_ptr<async_rpc::Targeter> makeTargeter(OperationContext* opCtx) final {
        return nullptr;
    }

    std::shared_ptr<executor::TaskExecutor> _executor;
};

class BackfillCoordinatorTest : public ShardingTestFixture {
public:
    void setUp() final {
        ShardingTestFixture::setUp();
        query_settings::QuerySettingsService::initializeForTest(getGlobalServiceContext());
        BackfillCoordinator::OnCompletionHook setPromise =
            [this](std::vector<QueryShapeHash> hashes,
                   LogicalTime time,
                   boost::optional<TenantId> tenantId) {
                // Ensure that 'expectFutureBackfill()' was called beforehand.
                ASSERT(optionalPromise.has_value());
                // Ensure that emplacing the promise won't throw any exceptions. Setting a promise
                // more than once will throw 'BrokenPromise'.
                ASSERT_DOES_NOT_THROW(optionalPromise->emplaceValue(
                    std::move(hashes), std::move(time), std::move(tenantId)));
            };
        _backfiilCoordinator =
            std::make_unique<BackfillCoordinatorForTest>(std::move(setPromise), executor());
    }

    boost::intrusive_ptr<ExpressionContext> expressionContext() {
        return makeBlankExpressionContext(operationContext(),
                                          NamespaceString::createNamespaceString_forTest(
                                              /* tenantId */ boost::none, "test", "test"));
    }

    BackfillCoordinator* coordinator() {
        return _backfiilCoordinator.get();
    }

    QuerySettingsService& service() {
        return QuerySettingsService::get(operationContext());
    }

    BackfillCoordinatorForTest::State* peekCoordinatorState() {
        return dynamic_cast<BackfillCoordinatorForTest*>(_backfiilCoordinator.get())->peekState();
    }

    /**
     * Set the 'insertRepresentativeQueriesToCollection()' implemenation to be used by the
     * BackfillCoordinator procedure.
     */
    void setInsertRepresentativeQueriesImpl(
        BackfillCoordinatorForTest::MockInsertRepresentativeQueriesFn
            onInsertRepresentativeQueries) {
        dynamic_cast<BackfillCoordinatorForTest*>(_backfiilCoordinator.get())
            ->mockInsertRepresentativeQueriesFn = onInsertRepresentativeQueries;
    }

    /**
     * Return a future 'BackfillCoordinator::OnCompletionHook' call result. Must be callled before
     * each backfill execution.
     */
    Future<BackfillOnCompletionHookResponse> expectFutureBackfillCompletion() {
        auto [promise, future] = makePromiseFuture<BackfillOnCompletionHookResponse>();
        optionalPromise = std::move(promise);
        return std::move(future);
    }

    /**
     * Advances the test clock time and executes the pending executor tasks.
     */
    void waitAndExecuteTasks(Seconds seconds) {
        executor::NetworkInterfaceMock::InNetworkGuard guard(network());
        network()->advanceTime(network()->now() + seconds);
    }

private:
    ServiceContext::UniqueOperationContext _opCtxHolder;
    std::unique_ptr<BackfillCoordinator> _backfiilCoordinator;
    boost::optional<Promise<BackfillOnCompletionHookResponse>> optionalPromise;
};

TEST_F(BackfillCoordinatorTest, ShouldmarkForBackfillAndScheduleIfNeeded) {
    auto expCtx = expressionContext();

    // It should backfill queries with missing representative queries.
    ASSERT_TRUE(BackfillCoordinator::shouldBackfill(expCtx, /* hasRepresentativeQuery*/ false));

    // It should skip backfilling queries which already have representative queries.
    ASSERT_FALSE(BackfillCoordinator::shouldBackfill(expCtx, /* hasRepresentativeQuery*/ true));

    // It should skip backfilling queries if they originate from an explain command.
    expCtx->setExplain(ExplainOptions::Verbosity::kQueryPlanner);
    ASSERT_FALSE(BackfillCoordinator::shouldBackfill(expCtx, /* hasRepresentativeQuery*/ false));

    // It should skip backfilling queries if 'internalQuerySettingsDisableBackfill' is set to true.
    RAIIServerParameterControllerForTest internalQuerySettingsDisableBackfill{
        "internalQuerySettingsDisableBackfill", true};
    expCtx->setExplain(boost::none);
    ASSERT_FALSE(BackfillCoordinator::shouldBackfill(expCtx, /* hasRepresentativeQuery*/ false));
}

TEST_F(BackfillCoordinatorTest, markForBackfillAndScheduleIfNeededShouldWaitBufferAndExecute) {
    RAIIServerParameterControllerForTest internalQuerySettingsBackfillDelaySeconds{
        "internalQuerySettingsBackfillDelaySeconds", 30};
    RAIIServerParameterControllerForTest internalQuerySettingsBackfillMemoryLimitBytes{
        "internalQuerySettingsBackfillMemoryLimitBytes", 16777248};
    const auto clusterParameterTime = LogicalTime(Timestamp(1234));
    std::string veryLargeQueryField(BSONObjMaxUserSize - 11, 'x');
    auto [bigQueryHash, bigQuery] = makeMockQueryShapeHashAndInstance(veryLargeQueryField);
    ASSERT_EQ(bigQuery.objsize(), BSONObjMaxUserSize);
    auto [hash0, query0] = makeMockQueryShapeHashAndInstance("Bob");
    auto [hash1, query1] = makeMockQueryShapeHashAndInstance("Alice");

    // Ensure that there are settings set over 'hash0' and 'hash1'.
    service().setAllQueryShapeConfigurations(
        QueryShapeConfigurationsWithTimestamp{.queryShapeConfigurations =
                                                  {
                                                      {hash0, QuerySettings()},
                                                      {hash1, QuerySettings()},
                                                      {bigQueryHash, QuerySettings()},
                                                  },
                                              .clusterParameterTime = clusterParameterTime},
        /* tenantId */ boost::none);
    ON_BLOCK_EXIT([&] { service().removeAllQueryShapeConfigurations(/* tenantId */ boost::none); });

    // Mark one query and expect it to be buffered and scheduled for
    // execution.
    auto future = expectFutureBackfillCompletion();
    coordinator()->markForBackfillAndScheduleIfNeeded(operationContext(), hash0, query0);
    auto state = peekCoordinatorState();
    ASSERT_TRUE(state->taskScheduled);
    ASSERT_EQ(state->buffer.size(), 1);
    ASSERT_BSONOBJ_EQ(state->buffer[hash0], query0);
    ASSERT_EQ(state->memoryUsedBytes, sizeof(QueryShapeHash) + query0.objsize());

    // Wait for 1 seconds and expect no backfill to be dispatched.
    waitAndExecuteTasks(Seconds{1});
    ASSERT_FALSE(future.isReady());

    // markForBackfillAndScheduleIfNeeded another query and expect it to be buffered.
    coordinator()->markForBackfillAndScheduleIfNeeded(operationContext(), hash1, query1);
    state = peekCoordinatorState();
    ASSERT_TRUE(state->taskScheduled);
    ASSERT_EQ(state->buffer.size(), 2);
    ASSERT_BSONOBJ_EQ(state->buffer[hash0], query0);
    ASSERT_BSONOBJ_EQ(state->buffer[hash1], query1);
    ASSERT_EQ(state->memoryUsedBytes,
              2 * sizeof(QueryShapeHash) + query0.objsize() + query1.objsize());

    // Wait for 1 more second and check that it still wasn't executed.
    waitAndExecuteTasks(Seconds{1});
    ASSERT_FALSE(future.isReady());

    // Try marking the same query again and expect no change in the current state.
    coordinator()->markForBackfillAndScheduleIfNeeded(operationContext(), hash0, query0);
    state = peekCoordinatorState();
    ASSERT_TRUE(state->taskScheduled);
    ASSERT_EQ(state->buffer.size(), 2);
    ASSERT_BSONOBJ_EQ(state->buffer[hash0], query0);
    ASSERT_BSONOBJ_EQ(state->buffer[hash1], query1);
    ASSERT_EQ(state->memoryUsedBytes,
              2 * sizeof(QueryShapeHash) + query0.objsize() + query1.objsize());
    ASSERT_FALSE(future.isReady());

    // Try marking a very large query which would exceed the memory limit and expect the backfill to
    // be immediately executed to allow for the current query to be buffered.
    setInsertRepresentativeQueriesImpl(
        [=](std::vector<QueryShapeRepresentativeQuery> representativeQueries) {
            // Expect only two representative queries to be inserted.
            ASSERT_EQ(representativeQueries.size(), 2);
            return std::vector<QueryShapeHash>{hash0, hash1};
        });
    coordinator()->markForBackfillAndScheduleIfNeeded(operationContext(), bigQueryHash, bigQuery);
    waitAndExecuteTasks(Seconds{1});
    ASSERT_TRUE(future.isReady());
    auto [backfilledHashes0, backfillLastModifiedTime0, _tenantId0] = future.get();
    ASSERT_EQ(backfilledHashes0[0], hash0);
    ASSERT_EQ(backfilledHashes0[1], hash1);
    ASSERT_EQ(backfillLastModifiedTime0, clusterParameterTime);

    // Ensure that the new large query was buffered and that the old scheduled task was not yet
    // executed.
    future = expectFutureBackfillCompletion();
    state = peekCoordinatorState();
    ASSERT_TRUE(state->taskScheduled);
    ASSERT_EQ(state->buffer.size(), 1);
    ASSERT_BSONOBJ_EQ(state->buffer[bigQueryHash], bigQuery);
    ASSERT_EQ(state->memoryUsedBytes, sizeof(QueryShapeHash) + bigQuery.objsize());
    ASSERT_FALSE(future.isReady());

    // Wait for the rest of 'internalQuerySettingsBackfillDelaySeconds' and expect the new backfill
    // to be now executed.
    setInsertRepresentativeQueriesImpl(
        [=](std::vector<QueryShapeRepresentativeQuery> representativeQueries) {
            // Expect only the large query to be inserted.
            ASSERT_EQ(representativeQueries.size(), 1);
            ASSERT_BSONOBJ_EQ(representativeQueries[0].getRepresentativeQuery(), bigQuery);
            ASSERT_EQ(representativeQueries[0].get_id(), bigQueryHash);
            return std::vector<QueryShapeHash>{bigQueryHash};
        });
    waitAndExecuteTasks(Seconds{30});
    ASSERT_TRUE(future.isReady());
    auto [backfilledHashes1, backfillLastModifiedTime1, _tenantId1] = future.get();
    ASSERT_EQ(backfilledHashes1[0], bigQueryHash);
    ASSERT_EQ(backfillLastModifiedTime1, clusterParameterTime);

    // Ensure that the current backfill state is now empty.
    state = peekCoordinatorState();
    ASSERT_FALSE(state->taskScheduled);
    ASSERT_TRUE(state->buffer.empty());
    ASSERT_EQ(state->memoryUsedBytes, 0);
}

TEST_F(BackfillCoordinatorTest, ExecuteDoesNotInsertQueriesWithoutSettings) {
    RAIIServerParameterControllerForTest internalQuerySettingsBackfillDelaySeconds{
        "internalQuerySettingsBackfillDelaySeconds", 30};

    // Start by setting some settings on 'hash, mark it and expect it to be buffered.
    auto future = expectFutureBackfillCompletion();
    const auto [hash, query] = makeMockQueryShapeHashAndInstance("SoonToBeRemoved");
    LogicalTime clusterParameterTime =
        service().getClusterParameterTime(/* tenantId */ boost::none);
    clusterParameterTime.addTicks(1);
    service().setAllQueryShapeConfigurations(
        QueryShapeConfigurationsWithTimestamp{
            .queryShapeConfigurations = {QueryShapeConfiguration{hash, QuerySettings()}},
            .clusterParameterTime = clusterParameterTime},
        /* tenantId */ boost::none);
    coordinator()->markForBackfillAndScheduleIfNeeded(operationContext(), hash, query);
    auto state = peekCoordinatorState();
    ASSERT_TRUE(state->taskScheduled);
    ASSERT_EQ(state->buffer.size(), 1);
    ASSERT_BSONOBJ_EQ(state->buffer[hash], query);

    // Remove all the settings, increment the current time to start the backfill operation, and
    // expect no inserts to happen and the BackfillCoordinator::OnCompletionHook callback to never
    // be called.
    service().removeAllQueryShapeConfigurations(/* tenantId */ boost::none);
    setInsertRepresentativeQueriesImpl([](auto&&) {
        // Fail if any inserts are dispatched.
        ASSERT(false);
        return std::vector<QueryShapeHash>{};
    });
    waitAndExecuteTasks(Seconds{30});
    ASSERT_FALSE(future.isReady());

    // Ensure that the current backfill state is now empty.
    state = peekCoordinatorState();
    ASSERT_FALSE(state->taskScheduled);
    ASSERT_TRUE(state->buffer.empty());
    ASSERT_EQ(state->memoryUsedBytes, 0);
}

TEST_F(BackfillCoordinatorTest, CancelStopsFutureTasks) {
    RAIIServerParameterControllerForTest internalQuerySettingsBackfillDelaySeconds{
        "internalQuerySettingsBackfillDelaySeconds", 30};
    auto future = expectFutureBackfillCompletion();
    setInsertRepresentativeQueriesImpl([](auto&&) {
        // Fail if any inserts are dispatched.
        ASSERT(false);
        return std::vector<QueryShapeHash>{};
    });

    // Mark a query for backfill and expect it to be buffered.
    const auto [hash, query] = makeMockQueryShapeHashAndInstance("SoonToBeRemoved");
    coordinator()->markForBackfillAndScheduleIfNeeded(operationContext(), hash, query);
    auto state = peekCoordinatorState();
    auto cancellationToken = state->cancellationSource.token();
    ASSERT_TRUE(state->taskScheduled);
    ASSERT_EQ(state->buffer.size(), 1);
    ASSERT_BSONOBJ_EQ(state->buffer[hash], query);

    // Cancel the execution and expect the state to be cleared and the pending tasks cancelled.
    coordinator()->cancel();
    state = peekCoordinatorState();
    ASSERT_FALSE(state->taskScheduled);
    ASSERT_EQ(state->buffer.size(), 0);
    ASSERT(cancellationToken.isCanceled());
    waitAndExecuteTasks(Seconds{30});
    ASSERT_FALSE(future.isReady());
}

TEST_F(BackfillCoordinatorTest, MarkForBackfillAndScheduleIfNeededDoesNotLeakErrors) {
    RAIIServerParameterControllerForTest internalQuerySettingsBackfillDelaySeconds{
        "internalQuerySettingsBackfillDelaySeconds", 30};
    auto future = expectFutureBackfillCompletion();
    setInsertRepresentativeQueriesImpl([](auto&&) {
        // Fail if any inserts are dispatched.
        ASSERT(false);
        return std::vector<QueryShapeHash>{};
    });

    // Start by markForBackfillAndScheduleIfNeededing a query and ensuring that it is present.
    const auto [luckyHash, luckyQuery] = makeMockQueryShapeHashAndInstance("LuckyQuery");
    coordinator()->markForBackfillAndScheduleIfNeeded(operationContext(), luckyHash, luckyQuery);
    auto state = peekCoordinatorState();
    auto cancellationToken = state->cancellationSource.token();
    ASSERT_TRUE(state->taskScheduled);
    ASSERT_EQ(state->buffer.size(), 1);
    ASSERT_BSONOBJ_EQ(state->buffer[luckyHash], luckyQuery);

    // Try marking a query bound to fail and expect no error to leak through. Ensure that
    // "BackfillCoordinator" is returned to a clean state and that any pending work is cancelled.
    const auto [unluckyHash, unluckyQuery] = makeMockQueryShapeHashAndInstance("UnluckyQuery");
    FailPointEnableBlock fp("throwBeforeSchedulingBackfillTask");
    ASSERT_DOES_NOT_THROW(coordinator()->markForBackfillAndScheduleIfNeeded(
        operationContext(), unluckyHash, unluckyQuery));
    state = peekCoordinatorState();
    ASSERT_FALSE(state->taskScheduled);
    ASSERT_EQ(state->buffer.size(), 0);
    ASSERT(cancellationToken.isCanceled());
    waitAndExecuteTasks(Seconds{30});
    ASSERT_FALSE(future.isReady());
}

}  // namespace
}  // namespace mongo::query_settings
