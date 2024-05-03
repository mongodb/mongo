/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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
#include "mongo/db/cursor_id.h"
#include "mongo/db/query/search/mongot_cursor_getmore_strategy.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"

namespace mongo {
namespace executor {
namespace {

class MongotCursorHelpersTest : public unittest::Test {
protected:
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("testdb.coll");

    const CursorId cursorId = 1;
};

TEST_F(MongotCursorHelpersTest, BatchSizeGrowsExponentiallyFromDefaultStartingSize) {
    MongotTaskExecutorCursorGetMoreStrategy getMoreStrategy =
        MongotTaskExecutorCursorGetMoreStrategy();

    // Check that GetMoreStrategy is properly initialized with the default values.
    ASSERT_EQ(101, getMoreStrategy.getCurrentBatchSize());
    ASSERT_EQ(std::vector<long long>{101}, getMoreStrategy.getBatchSizeHistory());

    // The first GetMore request should contain a batchSize of kDefaultMongotBatchSize *
    // kInternalSearchBatchSizeGrowthFactor.
    BSONObj getMoreRequest =
        getMoreStrategy.createGetMoreRequest(cursorId, nss, /*prevBatchNumReceived*/ 101);
    BSONObj expectedGetMoreRequest = BSON("getMore" << cursorId << "collection" << nss.coll()
                                                    << "cursorOptions" << BSON("batchSize" << 202));

    ASSERT_BSONOBJ_EQ(getMoreRequest, expectedGetMoreRequest);
    ASSERT_EQ(202, getMoreStrategy.getCurrentBatchSize());
    ASSERT_EQ(std::vector<long long>({101, 202}), getMoreStrategy.getBatchSizeHistory());

    // The next GetMore should exponentially increase the batchSize by
    // kInternalSearchBatchSizeFactor.
    getMoreRequest =
        getMoreStrategy.createGetMoreRequest(cursorId, nss, /*prevBatchNumReceived*/ 202);
    expectedGetMoreRequest = BSON("getMore" << cursorId << "collection" << nss.coll()
                                            << "cursorOptions" << BSON("batchSize" << 404));

    ASSERT_BSONOBJ_EQ(getMoreRequest, expectedGetMoreRequest);
    ASSERT_EQ(404, getMoreStrategy.getCurrentBatchSize());
    ASSERT_EQ(std::vector<long long>({101, 202, 404}), getMoreStrategy.getBatchSizeHistory());
}

TEST_F(MongotCursorHelpersTest, BatchSizeGrowsExponentiallyFromCustomStartingSize) {
    MongotTaskExecutorCursorGetMoreStrategy getMoreStrategy =
        MongotTaskExecutorCursorGetMoreStrategy(
            /*preFetchNextBatch*/ true, /*calcDocsNeededFn*/ nullptr, /*startingBatchSize*/ 3);

    // Check that GetMoreStrategy is properly initialized with our specified startingBatchSize
    // value.
    ASSERT_EQ(3, getMoreStrategy.getCurrentBatchSize());
    ASSERT_EQ(std::vector<long long>{3}, getMoreStrategy.getBatchSizeHistory());

    // The first GetMore request should contain a batchSize of startingBatchSize *
    // kInternalSearchBatchSizeGrowthFactor.
    BSONObj getMoreRequest =
        getMoreStrategy.createGetMoreRequest(cursorId, nss, /*prevBatchNumReceived*/ 3);
    BSONObj expectedGetMoreRequest = BSON("getMore" << cursorId << "collection" << nss.coll()
                                                    << "cursorOptions" << BSON("batchSize" << 6));

    ASSERT_BSONOBJ_EQ(getMoreRequest, expectedGetMoreRequest);
    ASSERT_EQ(6, getMoreStrategy.getCurrentBatchSize());
    ASSERT_EQ(std::vector<long long>({3, 6}), getMoreStrategy.getBatchSizeHistory());

    // The next GetMore should exponentially increase the batchSize by
    // kInternalSearchBatchSizeFactor.
    getMoreRequest =
        getMoreStrategy.createGetMoreRequest(cursorId, nss, /*prevBatchNumReceived*/ 6);
    expectedGetMoreRequest = BSON("getMore" << cursorId << "collection" << nss.coll()
                                            << "cursorOptions" << BSON("batchSize" << 12));

    ASSERT_BSONOBJ_EQ(getMoreRequest, expectedGetMoreRequest);
    ASSERT_EQ(12, getMoreStrategy.getCurrentBatchSize());
    ASSERT_EQ(std::vector<long long>({3, 6, 12}), getMoreStrategy.getBatchSizeHistory());
}

TEST_F(MongotCursorHelpersTest, BatchSizeGrowthPausesAndResumes) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagSearchBatchSizeTuning",
                                                               true);
    MongotTaskExecutorCursorGetMoreStrategy getMoreStrategy =
        MongotTaskExecutorCursorGetMoreStrategy(
            /*preFetchNextBatch*/ true, /*calcDocsNeededFn*/ nullptr, /*startingBatchSize*/ 10);

    // Check that GetMoreStrategy is properly initialized with our specified startingBatchSize
    // value.
    ASSERT_EQ(10, getMoreStrategy.getCurrentBatchSize());
    ASSERT_EQ(std::vector<long long>{10}, getMoreStrategy.getBatchSizeHistory());

    // The batchSize shouldn't increase if it only received 9 but requested 10.
    BSONObj getMoreRequest =
        getMoreStrategy.createGetMoreRequest(cursorId, nss, /*prevBatchNumReceived*/ 9);
    BSONObj expectedGetMoreRequest = BSON("getMore" << cursorId << "collection" << nss.coll()
                                                    << "cursorOptions" << BSON("batchSize" << 10));

    ASSERT_BSONOBJ_EQ(getMoreRequest, expectedGetMoreRequest);
    ASSERT_EQ(10, getMoreStrategy.getCurrentBatchSize());
    ASSERT_EQ(std::vector<long long>({10, 10}), getMoreStrategy.getBatchSizeHistory());

    // The next GetMore batchSize should exponentially increase if it received all 10 requested.
    getMoreRequest =
        getMoreStrategy.createGetMoreRequest(cursorId, nss, /*prevBatchNumReceived*/ 10);
    expectedGetMoreRequest = BSON("getMore" << cursorId << "collection" << nss.coll()
                                            << "cursorOptions" << BSON("batchSize" << 20));

    ASSERT_BSONOBJ_EQ(getMoreRequest, expectedGetMoreRequest);
    ASSERT_EQ(20, getMoreStrategy.getCurrentBatchSize());
    ASSERT_EQ(std::vector<long long>({10, 10, 20}), getMoreStrategy.getBatchSizeHistory());

    // The next GetMore batchSize should exponentially increase if it received all 20 requested.
    getMoreRequest =
        getMoreStrategy.createGetMoreRequest(cursorId, nss, /*prevBatchNumReceived*/ 20);
    expectedGetMoreRequest = BSON("getMore" << cursorId << "collection" << nss.coll()
                                            << "cursorOptions" << BSON("batchSize" << 40));

    ASSERT_BSONOBJ_EQ(getMoreRequest, expectedGetMoreRequest);
    ASSERT_EQ(40, getMoreStrategy.getCurrentBatchSize());
    ASSERT_EQ(std::vector<long long>({10, 10, 20, 40}), getMoreStrategy.getBatchSizeHistory());

    // batchSize growth should pause again if it doesn't receive all requested.
    getMoreRequest =
        getMoreStrategy.createGetMoreRequest(cursorId, nss, /*prevBatchNumReceived*/ 38);
    expectedGetMoreRequest = BSON("getMore" << cursorId << "collection" << nss.coll()
                                            << "cursorOptions" << BSON("batchSize" << 40));

    ASSERT_BSONOBJ_EQ(getMoreRequest, expectedGetMoreRequest);
    ASSERT_EQ(40, getMoreStrategy.getCurrentBatchSize());
    ASSERT_EQ(std::vector<long long>({10, 10, 20, 40, 40}), getMoreStrategy.getBatchSizeHistory());

    // batchSize growth should remain paused if it doesn't receive all requested.
    getMoreRequest =
        getMoreStrategy.createGetMoreRequest(cursorId, nss, /*prevBatchNumReceived*/ 39);
    expectedGetMoreRequest = BSON("getMore" << cursorId << "collection" << nss.coll()
                                            << "cursorOptions" << BSON("batchSize" << 40));

    ASSERT_BSONOBJ_EQ(getMoreRequest, expectedGetMoreRequest);
    ASSERT_EQ(40, getMoreStrategy.getCurrentBatchSize());
    ASSERT_EQ(std::vector<long long>({10, 10, 20, 40, 40, 40}),
              getMoreStrategy.getBatchSizeHistory());
}

/**
 * Tests that the GetMore request cannot contain both the "batchSize" and "docsRequested"
 * cursorOptions.
 */
DEATH_TEST_F(MongotCursorHelpersTest,
             GetMoreRequestCannotUseDefaultBatchSizeWithDocsRequested,
             "one of docsRequested or batchSize should be enabled") {
    auto calcDocsNeededFn = []() {
        return 10;
    };

    // Fails when we pass a calcDocsNeededFn and default batchSize.
    MongotTaskExecutorCursorGetMoreStrategy(/*preFetchNextBatch*/ true, calcDocsNeededFn);
}
DEATH_TEST_F(MongotCursorHelpersTest,
             GetMoreRequestCannotUseNonDefaultBatchSizeWithDocsRequested,
             "one of docsRequested or batchSize should be enabled") {
    auto calcDocsNeededFn = []() {
        return 10;
    };

    // Fails when we pass a calcDocsNeededFn and default batchSize.
    MongotTaskExecutorCursorGetMoreStrategy(
        /*preFetchNextBatch*/ true, calcDocsNeededFn, /*initialBatchSize*/ 10);
}

}  // namespace
}  // namespace executor
}  // namespace mongo
