// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/change_streams/data_to_shards_allocation_query_service_impl.h"

#include "mongo/db/pipeline/historical_placement_fetcher_mock.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <utility>

namespace mongo {

class DataToShardsAllocationQueryServiceImplTest : public ServiceContextTest {
public:
    DataToShardsAllocationQueryServiceImplTest()
        : ServiceContextTest(
              std::make_unique<ScopedGlobalServiceContextForTest>(false /* shouldSetupTL */)) {
        _opCtx = makeOperationContext();
    }

    auto getOpCtx() {
        return _opCtx.get();
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(DataToShardsAllocationQueryServiceImplTest,
       WhenFetcherReturnsStatus_ThenSameStatusIsReturned) {
    auto historicalFetcherMock = std::make_unique<HistoricalPlacementFetcherMock>();
    ASSERT_TRUE(historicalFetcherMock->empty());

    Timestamp ts(40, 1);
    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {ts, HistoricalPlacement({}, HistoricalPlacementStatus::OK)},
        {ts, HistoricalPlacement({}, HistoricalPlacementStatus::FutureClusterTime)},
        {ts, HistoricalPlacement({}, HistoricalPlacementStatus::NotAvailable)}};
    historicalFetcherMock->bufferResponses(responses);

    DataToShardsAllocationQueryServiceImpl impl(std::move(historicalFetcherMock));

    // Assert that returned AllocationToShardsStatus corresponds to the HistoricalPlacementStatus
    // returned from the fetcher.
    ASSERT_EQ(AllocationToShardsStatus::kOk, impl.getAllocationToShardsStatus(getOpCtx(), ts));
    ASSERT_EQ(AllocationToShardsStatus::kFutureClusterTime,
              impl.getAllocationToShardsStatus(getOpCtx(), ts));
    ASSERT_EQ(AllocationToShardsStatus::kNotAvailable,
              impl.getAllocationToShardsStatus(getOpCtx(), ts));
}

TEST_F(DataToShardsAllocationQueryServiceImplTest,
       WhenGetAlloctionToShardStatusIsCalled_ThenFetchIsCalledWithEmptyNamespaceAsArgument) {
    auto historicalFetcherMock = std::make_unique<HistoricalPlacementFetcherMock>();
    ASSERT_TRUE(historicalFetcherMock->empty());

    // Ensure that fetch() is called with NamespaceString::kEmpty.
    Timestamp ts(40, 1);
    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {NamespaceString::kEmpty, HistoricalPlacement({}, HistoricalPlacementStatus::OK)},
        {NamespaceString::kEmpty,
         HistoricalPlacement({}, HistoricalPlacementStatus::FutureClusterTime)},
        {NamespaceString::kEmpty, HistoricalPlacement({}, HistoricalPlacementStatus::NotAvailable)},
    };
    historicalFetcherMock->bufferResponses(responses);

    DataToShardsAllocationQueryServiceImpl impl(std::move(historicalFetcherMock));

    ASSERT_EQ(AllocationToShardsStatus::kOk, impl.getAllocationToShardsStatus(getOpCtx(), ts));
    ASSERT_EQ(AllocationToShardsStatus::kFutureClusterTime,
              impl.getAllocationToShardsStatus(getOpCtx(), ts));
    ASSERT_EQ(AllocationToShardsStatus::kNotAvailable,
              impl.getAllocationToShardsStatus(getOpCtx(), ts));
}

}  // namespace mongo
