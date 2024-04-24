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
    RAIIServerParameterControllerForTest featureFlagController("featureFlagSearchBatchSizeTuning",
                                                               true);
    MongotTaskExecutorCursorGetMoreStrategy getMoreStrategy =
        MongotTaskExecutorCursorGetMoreStrategy();

    // Check that GetMoreStrategy is properly initialized with the default values.
    ASSERT_EQ(101, getMoreStrategy.getCurrentBatchSize());
    ASSERT_EQ(std::vector<int64_t>{101}, getMoreStrategy.getBatchSizeHistory());

    // The first GetMore request should contain a batchSize of kDefaultMongotBatchSize *
    // kInternalSearchBatchSizeGrowthFactor.
    BSONObj getMoreRequest = getMoreStrategy.createGetMoreRequest(cursorId, nss);
    BSONObj expectedGetMoreRequest = BSON("getMore" << cursorId << "collection" << nss.coll()
                                                    << "cursorOptions" << BSON("batchSize" << 202));

    ASSERT_BSONOBJ_EQ(getMoreRequest, expectedGetMoreRequest);
    ASSERT_EQ(202, getMoreStrategy.getCurrentBatchSize());
    ASSERT_EQ(std::vector<int64_t>({101, 202}), getMoreStrategy.getBatchSizeHistory());

    // The next GetMore should exponentially increase the batchSize by
    // kInternalSearchBatchSizeFactor.
    getMoreRequest = getMoreStrategy.createGetMoreRequest(cursorId, nss);
    expectedGetMoreRequest = BSON("getMore" << cursorId << "collection" << nss.coll()
                                            << "cursorOptions" << BSON("batchSize" << 404));

    ASSERT_BSONOBJ_EQ(getMoreRequest, expectedGetMoreRequest);
    ASSERT_EQ(404, getMoreStrategy.getCurrentBatchSize());
    ASSERT_EQ(std::vector<int64_t>({101, 202, 404}), getMoreStrategy.getBatchSizeHistory());
}

TEST_F(MongotCursorHelpersTest, BatchSizeGrowsExponentiallyFromCustomStartingSize) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagSearchBatchSizeTuning",
                                                               true);
    MongotTaskExecutorCursorGetMoreStrategy getMoreStrategy =
        MongotTaskExecutorCursorGetMoreStrategy(
            /*preFetchNextBatch*/ true, /*calcDocsNeededFn*/ nullptr, /*startingBatchSize*/ 3);

    // Check that GetMoreStrategy is properly initialized with our specified startingBatchSize
    // value.
    ASSERT_EQ(3, getMoreStrategy.getCurrentBatchSize());
    ASSERT_EQ(std::vector<int64_t>{3}, getMoreStrategy.getBatchSizeHistory());

    // The first GetMore request should contain a batchSize of startingBatchSize *
    // kInternalSearchBatchSizeGrowthFactor.
    BSONObj getMoreRequest = getMoreStrategy.createGetMoreRequest(cursorId, nss);
    BSONObj expectedGetMoreRequest = BSON("getMore" << cursorId << "collection" << nss.coll()
                                                    << "cursorOptions" << BSON("batchSize" << 6));

    ASSERT_BSONOBJ_EQ(getMoreRequest, expectedGetMoreRequest);
    ASSERT_EQ(6, getMoreStrategy.getCurrentBatchSize());
    ASSERT_EQ(std::vector<int64_t>({3, 6}), getMoreStrategy.getBatchSizeHistory());

    // The next GetMore should exponentially increase the batchSize by
    // kInternalSearchBatchSizeFactor.
    getMoreRequest = getMoreStrategy.createGetMoreRequest(cursorId, nss);
    expectedGetMoreRequest = BSON("getMore" << cursorId << "collection" << nss.coll()
                                            << "cursorOptions" << BSON("batchSize" << 12));

    ASSERT_BSONOBJ_EQ(getMoreRequest, expectedGetMoreRequest);
    ASSERT_EQ(12, getMoreStrategy.getCurrentBatchSize());
    ASSERT_EQ(std::vector<int64_t>({3, 6, 12}), getMoreStrategy.getBatchSizeHistory());
}

/**
 * Tests that the GetMore request cannot contain both the "batchSize" and "docsRequested"
 * cursorOptions.
 */
TEST_F(MongotCursorHelpersTest, GetMoreRequestContainsEitherDocsRequestedOrBatchSize) {
    RAIIServerParameterControllerForTest batchSizeTuningFeatureFlagController(
        "featureFlagSearchBatchSizeTuning", true);
    RAIIServerParameterControllerForTest batchSizeLimitFeatureFlagController(
        "featureFlagSearchBatchSizeLimit", true);
    auto calcDocsNeededFn = []() {
        return 10;
    };

    MongotTaskExecutorCursorGetMoreStrategy getMoreStrategy =
        MongotTaskExecutorCursorGetMoreStrategy(/*preFetchNextBatch*/ true, calcDocsNeededFn);

    BSONObj getMoreRequest = getMoreStrategy.createGetMoreRequest(cursorId, nss);
    BSONObj expectedGetMoreRequest = BSON("getMore" << cursorId << "collection" << nss.coll()
                                                    << "cursorOptions" << BSON("batchSize" << 202));

    ASSERT_BSONOBJ_EQ(getMoreRequest, expectedGetMoreRequest);
}

}  // namespace
}  // namespace executor
}  // namespace mongo
