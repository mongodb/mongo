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
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/search/mongot_cursor_getmore_strategy.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

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
    BSONObj getMoreRequest = getMoreStrategy.createGetMoreRequest(
        cursorId, nss, /*prevBatchNumReceived*/ 101, /*totalNumReceived*/ 101);
    BSONObj expectedGetMoreRequest = BSON("getMore" << cursorId << "collection" << nss.coll()
                                                    << "cursorOptions" << BSON("batchSize" << 152));

    ASSERT_BSONOBJ_EQ(getMoreRequest, expectedGetMoreRequest);
    ASSERT_EQ(152, getMoreStrategy.getCurrentBatchSize());
    ASSERT_EQ(std::vector<long long>({101, 152}), getMoreStrategy.getBatchSizeHistory());

    // The next GetMore should exponentially increase the batchSize by
    // kInternalSearchBatchSizeFactor.
    getMoreRequest = getMoreStrategy.createGetMoreRequest(
        cursorId, nss, /*prevBatchNumReceived*/ 152, /*totalNumReceived*/ 253);
    expectedGetMoreRequest = BSON("getMore" << cursorId << "collection" << nss.coll()
                                            << "cursorOptions" << BSON("batchSize" << 228));

    ASSERT_BSONOBJ_EQ(getMoreRequest, expectedGetMoreRequest);
    ASSERT_EQ(228, getMoreStrategy.getCurrentBatchSize());
    ASSERT_EQ(std::vector<long long>({101, 152, 228}), getMoreStrategy.getBatchSizeHistory());
}

TEST_F(MongotCursorHelpersTest, BatchSizeGrowsExponentiallyFromCustomStartingSize) {
    MongotTaskExecutorCursorGetMoreStrategy getMoreStrategy =
        MongotTaskExecutorCursorGetMoreStrategy(/*startingBatchSize*/ 3);

    // Check that GetMoreStrategy is properly initialized with our specified startingBatchSize
    // value.
    ASSERT_EQ(3, getMoreStrategy.getCurrentBatchSize());
    ASSERT_EQ(std::vector<long long>{3}, getMoreStrategy.getBatchSizeHistory());

    // The first GetMore request should contain a batchSize of startingBatchSize *
    // kInternalSearchBatchSizeGrowthFactor.
    BSONObj getMoreRequest = getMoreStrategy.createGetMoreRequest(
        cursorId, nss, /*prevBatchNumReceived*/ 3, /*totalNumReceived*/ 3);
    BSONObj expectedGetMoreRequest = BSON("getMore" << cursorId << "collection" << nss.coll()
                                                    << "cursorOptions" << BSON("batchSize" << 5));

    ASSERT_BSONOBJ_EQ(getMoreRequest, expectedGetMoreRequest);
    ASSERT_EQ(5, getMoreStrategy.getCurrentBatchSize());
    ASSERT_EQ(std::vector<long long>({3, 5}), getMoreStrategy.getBatchSizeHistory());

    // The next GetMore should exponentially increase the batchSize by
    // kInternalSearchBatchSizeFactor.
    getMoreRequest = getMoreStrategy.createGetMoreRequest(
        cursorId, nss, /*prevBatchNumReceived*/ 5, /*totalNumReceived*/ 8);
    expectedGetMoreRequest = BSON("getMore" << cursorId << "collection" << nss.coll()
                                            << "cursorOptions" << BSON("batchSize" << 8));

    ASSERT_BSONOBJ_EQ(getMoreRequest, expectedGetMoreRequest);
    ASSERT_EQ(8, getMoreStrategy.getCurrentBatchSize());
    ASSERT_EQ(std::vector<long long>({3, 5, 8}), getMoreStrategy.getBatchSizeHistory());
}

TEST_F(MongotCursorHelpersTest, BatchSizeGrowthPausesAndResumes) {
    MongotTaskExecutorCursorGetMoreStrategy getMoreStrategy =
        MongotTaskExecutorCursorGetMoreStrategy(/*startingBatchSize*/ 10);

    // Check that GetMoreStrategy is properly initialized with our specified startingBatchSize
    // value.
    ASSERT_EQ(10, getMoreStrategy.getCurrentBatchSize());
    ASSERT_EQ(std::vector<long long>{10}, getMoreStrategy.getBatchSizeHistory());

    // The batchSize shouldn't increase if it only received 9 but requested 10.
    BSONObj getMoreRequest = getMoreStrategy.createGetMoreRequest(
        cursorId, nss, /*prevBatchNumReceived*/ 9, /*totalNumReceived*/ 9);
    BSONObj expectedGetMoreRequest = BSON("getMore" << cursorId << "collection" << nss.coll()
                                                    << "cursorOptions" << BSON("batchSize" << 10));

    ASSERT_BSONOBJ_EQ(getMoreRequest, expectedGetMoreRequest);
    ASSERT_EQ(10, getMoreStrategy.getCurrentBatchSize());
    ASSERT_EQ(std::vector<long long>({10, 10}), getMoreStrategy.getBatchSizeHistory());

    // The next GetMore batchSize should exponentially increase if it received all 10 requested.
    getMoreRequest = getMoreStrategy.createGetMoreRequest(
        cursorId, nss, /*prevBatchNumReceived*/ 10, /*totalNumReceived*/ 19);
    expectedGetMoreRequest = BSON("getMore" << cursorId << "collection" << nss.coll()
                                            << "cursorOptions" << BSON("batchSize" << 15));

    ASSERT_BSONOBJ_EQ(getMoreRequest, expectedGetMoreRequest);
    ASSERT_EQ(15, getMoreStrategy.getCurrentBatchSize());
    ASSERT_EQ(std::vector<long long>({10, 10, 15}), getMoreStrategy.getBatchSizeHistory());

    // The next GetMore batchSize should exponentially increase if it received all requested.
    getMoreRequest = getMoreStrategy.createGetMoreRequest(
        cursorId, nss, /*prevBatchNumReceived*/ 15, /*totalNumReceived*/ 34);
    expectedGetMoreRequest = BSON("getMore" << cursorId << "collection" << nss.coll()
                                            << "cursorOptions" << BSON("batchSize" << 23));

    ASSERT_BSONOBJ_EQ(getMoreRequest, expectedGetMoreRequest);
    ASSERT_EQ(23, getMoreStrategy.getCurrentBatchSize());
    ASSERT_EQ(std::vector<long long>({10, 10, 15, 23}), getMoreStrategy.getBatchSizeHistory());

    // batchSize growth should pause again if it doesn't receive all requested.
    getMoreRequest = getMoreStrategy.createGetMoreRequest(
        cursorId, nss, /*prevBatchNumReceived*/ 20, /*totalNumReceived*/ 54);
    expectedGetMoreRequest = BSON("getMore" << cursorId << "collection" << nss.coll()
                                            << "cursorOptions" << BSON("batchSize" << 23));

    ASSERT_BSONOBJ_EQ(getMoreRequest, expectedGetMoreRequest);
    ASSERT_EQ(23, getMoreStrategy.getCurrentBatchSize());
    ASSERT_EQ(std::vector<long long>({10, 10, 15, 23, 23}), getMoreStrategy.getBatchSizeHistory());

    // batchSize growth should remain paused if it doesn't receive all requested.
    getMoreRequest = getMoreStrategy.createGetMoreRequest(
        cursorId, nss, /*prevBatchNumReceived*/ 22, /*totalNumReceived*/ 76);
    expectedGetMoreRequest = BSON("getMore" << cursorId << "collection" << nss.coll()
                                            << "cursorOptions" << BSON("batchSize" << 23));

    ASSERT_BSONOBJ_EQ(getMoreRequest, expectedGetMoreRequest);
    ASSERT_EQ(23, getMoreStrategy.getCurrentBatchSize());
    ASSERT_EQ(std::vector<long long>({10, 10, 15, 23, 23, 23}),
              getMoreStrategy.getBatchSizeHistory());
}

TEST_F(MongotCursorHelpersTest, DefaultStrategyPrefetchAfterThreeBatches) {
    MongotTaskExecutorCursorGetMoreStrategy getMoreStrategy =
        MongotTaskExecutorCursorGetMoreStrategy();
    ASSERT_FALSE(
        getMoreStrategy.shouldPrefetch(/*totalNumReceived*/ 101, /*numBatchesReceived*/ 1));
    ASSERT_FALSE(
        getMoreStrategy.shouldPrefetch(/*totalNumReceived*/ 101, /*numBatchesReceived*/ 2));
    ASSERT_TRUE(getMoreStrategy.shouldPrefetch(/*totalNumReceived*/ 101, /*numBatchesReceived*/ 3));
    ASSERT_TRUE(getMoreStrategy.shouldPrefetch(/*totalNumReceived*/ 101, /*numBatchesReceived*/ 4));
    ASSERT_TRUE(getMoreStrategy.shouldPrefetch(/*totalNumReceived*/ 101, /*numBatchesReceived*/ 5));
}

TEST_F(MongotCursorHelpersTest, DiscreteMinBoundsUnknownMaxBoundsPrefetch) {
    // With discrete min bounds, prefetch if num received is less than minBounds. With unknown max
    // bounds, still prefetch always after third batch.
    MongotTaskExecutorCursorGetMoreStrategy getMoreStrategy =
        MongotTaskExecutorCursorGetMoreStrategy(
            /*startingBatchSize*/ 101, DocsNeededBounds(50, docs_needed_bounds::Unknown()));
    ASSERT_TRUE(getMoreStrategy.shouldPrefetch(/*totalNumReceived*/ 30, /*numBatchesReceived*/ 1));
    ASSERT_FALSE(getMoreStrategy.shouldPrefetch(/*totalNumReceived*/ 50, /*numBatchesReceived*/ 1));
    ASSERT_FALSE(getMoreStrategy.shouldPrefetch(/*totalNumReceived*/ 70, /*numBatchesReceived*/ 1));

    ASSERT_TRUE(getMoreStrategy.shouldPrefetch(/*totalNumReceived*/ 30, /*numBatchesReceived*/ 2));
    ASSERT_FALSE(getMoreStrategy.shouldPrefetch(/*totalNumReceived*/ 50, /*numBatchesReceived*/ 2));
    ASSERT_FALSE(getMoreStrategy.shouldPrefetch(/*totalNumReceived*/ 70, /*numBatchesReceived*/ 2));

    ASSERT_TRUE(getMoreStrategy.shouldPrefetch(/*totalNumReceived*/ 30, /*numBatchesReceived*/ 3));
    ASSERT_TRUE(getMoreStrategy.shouldPrefetch(/*totalNumReceived*/ 50, /*numBatchesReceived*/ 3));
    ASSERT_TRUE(getMoreStrategy.shouldPrefetch(/*totalNumReceived*/ 70, /*numBatchesReceived*/ 3));
}

TEST_F(MongotCursorHelpersTest, DiscreteMinBoundsDiscreteMaxBoundsPrefetch) {
    // With discrete min bounds and max bounds, only prefetch if num received is less than
    // minBounds.
    MongotTaskExecutorCursorGetMoreStrategy getMoreStrategy =
        MongotTaskExecutorCursorGetMoreStrategy(
            /*startingBatchSize*/ 101, DocsNeededBounds(50, 150));
    ASSERT_TRUE(getMoreStrategy.shouldPrefetch(/*totalNumReceived*/ 40, /*numBatchesReceived*/ 1));
    ASSERT_TRUE(getMoreStrategy.shouldPrefetch(/*totalNumReceived*/ 49, /*numBatchesReceived*/ 1));
    ASSERT_FALSE(
        getMoreStrategy.shouldPrefetch(/*totalNumReceived*/ 100, /*numBatchesReceived*/ 1));
    ASSERT_FALSE(
        getMoreStrategy.shouldPrefetch(/*totalNumReceived*/ 150, /*numBatchesReceived*/ 1));

    ASSERT_TRUE(getMoreStrategy.shouldPrefetch(/*totalNumReceived*/ 40, /*numBatchesReceived*/ 2));
    ASSERT_TRUE(getMoreStrategy.shouldPrefetch(/*totalNumReceived*/ 49, /*numBatchesReceived*/ 2));
    ASSERT_FALSE(
        getMoreStrategy.shouldPrefetch(/*totalNumReceived*/ 100, /*numBatchesReceived*/ 2));
    ASSERT_FALSE(
        getMoreStrategy.shouldPrefetch(/*totalNumReceived*/ 150, /*numBatchesReceived*/ 2));

    ASSERT_TRUE(getMoreStrategy.shouldPrefetch(/*totalNumReceived*/ 40, /*numBatchesReceived*/ 3));
    ASSERT_TRUE(getMoreStrategy.shouldPrefetch(/*totalNumReceived*/ 49, /*numBatchesReceived*/ 3));
    ASSERT_FALSE(
        getMoreStrategy.shouldPrefetch(/*totalNumReceived*/ 100, /*numBatchesReceived*/ 3));
    ASSERT_FALSE(
        getMoreStrategy.shouldPrefetch(/*totalNumReceived*/ 150, /*numBatchesReceived*/ 3));
}

TEST_F(MongotCursorHelpersTest, AlwaysPrefetchNeedAllBounds) {
    MongotTaskExecutorCursorGetMoreStrategy getMoreStrategy =
        MongotTaskExecutorCursorGetMoreStrategy(
            /*startingBatchSize*/ 101,
            DocsNeededBounds(docs_needed_bounds::NeedAll(), docs_needed_bounds::NeedAll()));
    ASSERT_TRUE(getMoreStrategy.shouldPrefetch(/*totalNumReceived*/ 0, /*numBatchesReceived*/ 1));
    ASSERT_TRUE(getMoreStrategy.shouldPrefetch(/*totalNumReceived*/ 50, /*numBatchesReceived*/ 1));
    ASSERT_TRUE(getMoreStrategy.shouldPrefetch(/*totalNumReceived*/ 101, /*numBatchesReceived*/ 1));

    ASSERT_TRUE(getMoreStrategy.shouldPrefetch(/*totalNumReceived*/ 0, /*numBatchesReceived*/ 2));
    ASSERT_TRUE(getMoreStrategy.shouldPrefetch(/*totalNumReceived*/ 50, /*numBatchesReceived*/ 2));
    ASSERT_TRUE(getMoreStrategy.shouldPrefetch(/*totalNumReceived*/ 101, /*numBatchesReceived*/ 2));

    ASSERT_TRUE(getMoreStrategy.shouldPrefetch(/*totalNumReceived*/ 0, /*numBatchesReceived*/ 3));
    ASSERT_TRUE(getMoreStrategy.shouldPrefetch(/*totalNumReceived*/ 50, /*numBatchesReceived*/ 3));
    ASSERT_TRUE(getMoreStrategy.shouldPrefetch(/*totalNumReceived*/ 101, /*numBatchesReceived*/ 3));
}

/*
 * This test checks that in a non-stored-source search query with an extractable limit
 * that the batch size sent to mongot is adjusted properly when not all the documents sent back
 * from mongot are found in mongod.
 */
TEST_F(MongotCursorHelpersTest, QueryHasExtractableLimitAndNotAllDocsFoundInLookup) {
    long long extractableLimit = 100;
    long long startingBatchSize = 101;
    double defaultOversubscriptionFactor = 1.064;

    std::shared_ptr<DocumentSourceInternalSearchIdLookUp::SearchIdLookupMetrics>
        searchIdLookupMetrics =
            std::make_shared<DocumentSourceInternalSearchIdLookUp::SearchIdLookupMetrics>();

    MongotTaskExecutorCursorGetMoreStrategy getMoreStrategy =
        MongotTaskExecutorCursorGetMoreStrategy(
            startingBatchSize,
            DocsNeededBounds(extractableLimit, extractableLimit),
            /*tenantId*/ boost::none,
            searchIdLookupMetrics);

    ASSERT_EQ(startingBatchSize, getMoreStrategy.getCurrentBatchSize());

    // First, start with the unexpected case where for whatever reason no docs were found in id
    // lookup, so that our batch size computation does not divide by zero.
    for (int i = 0; i < getMoreStrategy.getCurrentBatchSize(); ++i) {
        searchIdLookupMetrics->incrementDocsSeenByIdLookup();
    }
    getMoreStrategy.createGetMoreRequest(
        cursorId, nss, /*prevBatchNumReceived*/ 101, /*totalNumReceived*/ 101);
    ASSERT_EQ(152, getMoreStrategy.getCurrentBatchSize());

    // Now, add in some realistic search id lookup metrics where some,
    // but not all docs were found, and assert that the batch size updates as expected.
    for (int i = 0; i < getMoreStrategy.getCurrentBatchSize(); ++i) {
        searchIdLookupMetrics->incrementDocsSeenByIdLookup();
    }
    for (int i = 0; i < extractableLimit * 0.7; ++i) {
        searchIdLookupMetrics->incrementDocsReturnedByIdLookup();
    }

    // Now we expect to fall into the special case where the batch size grows by a factor
    // relative to the success rate of how many docs have been seen vs returned in id lookup.
    getMoreStrategy.createGetMoreRequest(
        cursorId, nss, /*prevBatchNumReceived*/ 152, /*totalNumReceived*/ 253);

    long long nDocsStillNeeded = extractableLimit - (extractableLimit * 0.7);  // 30
    double idLookupSuccessRate = (extractableLimit * 0.7) / 253;               // ~0.2767
    long long expectedBatchSize =
        defaultOversubscriptionFactor * (nDocsStillNeeded / idLookupSuccessRate);  // 115
    ASSERT_EQ(expectedBatchSize, getMoreStrategy.getCurrentBatchSize());
}
}  // namespace
}  // namespace executor
}  // namespace mongo
