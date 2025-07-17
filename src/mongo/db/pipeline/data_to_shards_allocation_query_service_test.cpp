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
