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
