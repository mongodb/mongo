// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/data_to_shards_allocation_query_service.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/data_to_shards_allocation_query_service_mock.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <utility>
#include <vector>

namespace mongo {

class DataToShardsAllocationQueryServiceTest : public ServiceContextTest {
public:
    DataToShardsAllocationQueryServiceTest()
        : ServiceContextTest(
              std::make_unique<ScopedGlobalServiceContextForTest>(false /* shouldSetupTL */)) {
        _opCtx = makeOperationContext();
    }

    auto getOpCtx() {
        return _opCtx.get();
    }

    DataToShardsAllocationQueryServiceMock mock;

private:
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(DataToShardsAllocationQueryServiceTest, MockReturnsCorrectStatuses) {
    ASSERT_TRUE(mock.empty());

    // Fill the buffer with 4 expected responses.
    std::vector<DataToShardsAllocationQueryServiceMock::Response> responses;
    responses.push_back(std::make_pair(Timestamp(40, 1), AllocationToShardsStatus::kNotAvailable));
    responses.push_back(std::make_pair(Timestamp(40, 2), AllocationToShardsStatus::kOk));
    responses.push_back(std::make_pair(Timestamp(41, 0), AllocationToShardsStatus::kOk));
    responses.push_back(
        std::make_pair(Timestamp(41, 2), AllocationToShardsStatus::kFutureClusterTime));

    responses.push_back(
        std::make_pair(Timestamp(41, 3), AllocationToShardsStatus::kFutureClusterTime));

    mock.bufferResponses(responses);

    // Call the mock with 4 times, with the expected timestamps. This should work.
    ASSERT_EQ(AllocationToShardsStatus::kNotAvailable,
              mock.getAllocationToShardsStatus(getOpCtx(), Timestamp(40, 1)));
    ASSERT_EQ(AllocationToShardsStatus::kOk,
              mock.getAllocationToShardsStatus(getOpCtx(), Timestamp(40, 2)));
    ASSERT_EQ(AllocationToShardsStatus::kOk,
              mock.getAllocationToShardsStatus(getOpCtx(), Timestamp(41, 0)));
    ASSERT_EQ(AllocationToShardsStatus::kFutureClusterTime,
              mock.getAllocationToShardsStatus(getOpCtx(), Timestamp(41, 2)));
    ASSERT_EQ(AllocationToShardsStatus::kFutureClusterTime,
              mock.getAllocationToShardsStatus(getOpCtx(), Timestamp(41, 3)));

    ASSERT_TRUE(mock.empty());
}

TEST_F(DataToShardsAllocationQueryServiceTest, MockFailsOnEmptyQueue) {
    ASSERT_THROWS_CODE(mock.getAllocationToShardsStatus(getOpCtx(), Timestamp(42, 0)),
                       AssertionException,
                       10612400);
}

TEST_F(DataToShardsAllocationQueryServiceTest, MockThrowsOnUnexpectedClusterTime) {
    ASSERT_TRUE(mock.empty());

    std::vector<DataToShardsAllocationQueryServiceMock::Response> responses;
    responses.push_back(std::make_pair(Timestamp(40, 1), AllocationToShardsStatus::kNotAvailable));
    responses.push_back(std::make_pair(Timestamp(40, 2), AllocationToShardsStatus::kOk));

    mock.bufferResponses(responses);

    // Call with a different cluster time than expected. This will throw.
    ASSERT_THROWS_CODE(mock.getAllocationToShardsStatus(getOpCtx(), Timestamp(42, 0)),
                       AssertionException,
                       10612401);

    // Call with a expected cluster time. This should work fine.
    ASSERT_EQ(AllocationToShardsStatus::kNotAvailable,
              mock.getAllocationToShardsStatus(getOpCtx(), Timestamp(40, 1)));

    // Call again with a different cluster time than expected. This will throw.
    ASSERT_THROWS_CODE(mock.getAllocationToShardsStatus(getOpCtx(), Timestamp(40, 1)),
                       AssertionException,
                       10612401);

    // Call with a expected cluster time. This should work fine.
    ASSERT_EQ(AllocationToShardsStatus::kOk,
              mock.getAllocationToShardsStatus(getOpCtx(), Timestamp(40, 2)));
}

TEST_F(DataToShardsAllocationQueryServiceTest, DecorationsOnServiceContextWork) {
    std::vector<DataToShardsAllocationQueryServiceMock::Response> responses;
    responses.push_back(std::make_pair(Timestamp(40, 1), AllocationToShardsStatus::kNotAvailable));
    responses.push_back(std::make_pair(Timestamp(40, 2), AllocationToShardsStatus::kOk));

    auto decoratedMock = std::make_unique<DataToShardsAllocationQueryServiceMock>();
    decoratedMock->bufferResponses(responses);

    // Register the mock with the service context.
    DataToShardsAllocationQueryService::set(getServiceContext(), std::move(decoratedMock));

    // Call the decoration on the OperationContext.
    ASSERT_EQ(AllocationToShardsStatus::kNotAvailable,
              DataToShardsAllocationQueryService::get(getOpCtx())
                  ->getAllocationToShardsStatus(getOpCtx(), Timestamp(40, 1)));

    // Call the decoration on the ServiceContext.
    ASSERT_EQ(AllocationToShardsStatus::kOk,
              DataToShardsAllocationQueryService::get(getServiceContext())
                  ->getAllocationToShardsStatus(getOpCtx(), Timestamp(40, 2)));
}

}  // namespace mongo
