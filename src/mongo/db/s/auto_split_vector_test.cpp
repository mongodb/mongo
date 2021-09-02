/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/s/auto_split_vector.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/s/split_vector.h"

namespace mongo {
namespace {

const NamespaceString kNss = NamespaceString("autosplitDB", "coll");
const std::string kPattern = "_id";

/*
 * Insert the specified number of documents in the test collection, with incremental shard key `_id`
 * starting from `startShardKey`.
 */
void insertNDocsOf1MB(OperationContext* opCtx, int nDocs, int startShardKey) {
    DBDirectClient client(opCtx);

    std::string oneMBstr(1024 * 1024, 'a');
    for (int i = startShardKey; i < startShardKey + 100; i++) {
        BSONObjBuilder builder;
        builder.append(kPattern, i);
        builder.append("str", oneMBstr);
        BSONObj obj = builder.obj();
        client.insert(kNss.toString(), obj);
    }
}

/*
 * Call the autoSplitVector function of the test collection on a chunk with bounds [0, 100) and with
 * the specified `maxChunkSizeMB`.
 */
std::vector<BSONObj> autoSplit(OperationContext* opCtx, int maxChunkSizeMB) {
    return autoSplitVector(opCtx,
                           kNss,
                           BSON(kPattern << 1) /* shard key pattern */,
                           BSON(kPattern << 0) /* min */,
                           BSON(kPattern << 100) /* max */,
                           maxChunkSizeMB * 1024 * 1024 /* max chunk size in bytes*/);
}

class AutoSplitVectorTest : public ShardServerTestFixture {
public:
    /*
     * Before each test case:
     * - Creates a sharded collection with shard key `_id`
     * - Inserts 100 documents of ~1MB size (shard keys [0...99])
     */
    void setUp() {
        ShardServerTestFixture::setUp();

        auto opCtx = operationContext();

        {
            OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE
                unsafeCreateCollection(opCtx);
            uassertStatusOK(createCollection(
                operationContext(), kNss.db().toString(), BSON("create" << kNss.coll())));
        }

        DBDirectClient client(opCtx);
        client.createIndex(kNss.ns(), BSON(kPattern << 1));

        int nDocs = 100;
        int startShardKey = 0;
        insertNDocsOf1MB(opCtx, nDocs, startShardKey);
        ASSERT_EQUALS(100ULL, client.count(kNss));
    }
};

// No split points if estimated data size < max chunk size
TEST_F(AutoSplitVectorTest, NoSplitIfDataLessThanMaxChunkSize) {
    std::vector<BSONObj> splitKeys = autoSplit(operationContext(), 101 /* maxChunkSizeMB */);
    ASSERT_EQ(splitKeys.size(), 0);
}

// No split points if max chunk size >> actual chunk size
TEST_F(AutoSplitVectorTest, NoSplitPoints) {
    std::vector<BSONObj> splitKeys = autoSplit(operationContext(), 256 /* maxChunkSizeMB */);
    ASSERT_EQUALS(splitKeys.size(), 0UL);
}

// Split point exactly in the middle in case of chunk size == maxChunkSize
TEST_F(AutoSplitVectorTest, SplitAtHalfChunkSizeIsMaxChunkSize) {
    std::vector<BSONObj> splitKeys = autoSplit(operationContext(), 100 /* maxChunkSizeMB */);
    BSONObj expectedSplitPoint = BSON(kPattern << 49);
    ASSERT_EQ(splitKeys.size(), 1);
    ASSERT_BSONOBJ_EQ(splitKeys.at(0), expectedSplitPoint);
}

/*
 * Check that the number of resulting chunks is equal to `(chunkSize / maxChunkSize) * 2`.
 *
 * `maxChunkSize` = 20MB
 * `chunkSize` = 100MB
 * Expected number of chunks: 10. Expected number of split points: 9.
 */
TEST_F(AutoSplitVectorTest, SplitAtHalfMaxChunkSize) {
    std::vector<BSONObj> splitKeys = autoSplit(operationContext(), 20 /* maxChunkSizeMB */);
    ASSERT_EQ(splitKeys.size(), 9);
    auto expectedSplitPoint = 9;
    for (const auto& splitPoint : splitKeys) {
        ASSERT_EQ(splitPoint.getIntField(kPattern), expectedSplitPoint);
        expectedSplitPoint += 10;
    }
}

/*
 * Check that a bigger last chunk is kept if its size is less than 90% maxChunkSize.
 *
 * `maxChunkSize` = 80MB
 * `chunkSize` = 100MB
 * Expected number of chunks: 2. Expected number of split points: 1.
 */
TEST_F(AutoSplitVectorTest, AvoidCreatingSmallChunks) {
    std::vector<BSONObj> splitKeys = autoSplit(operationContext(), 80 /* maxChunkSizeMB */);
    BSONObj expectedSplitPoint = BSON(kPattern << 39);
    ASSERT_EQ(splitKeys.size(), 1);
    ASSERT_BSONOBJ_EQ(splitKeys.at(0), expectedSplitPoint);
}

/*
 * Choose fairly the last split point if the last chunk size would be greater or equal than 90%
 * maxChunkSize.
 *
 * `maxChunkSize` = 110MB
 * `chunkSize` = 100MB
 * Expected number of chunks: 2. Expected number of split points: 1.
 */
TEST_F(AutoSplitVectorTest, FairLastSplitPoint) {
    {
        // Increase collection size so that the auto splitter can actually be triggered. Use a
        // different range to don't interfere with the chunk getting splitted.
        insertNDocsOf1MB(operationContext(), 20 /* nDocs */, 1024 /* startShardKey */);
    }

    std::vector<BSONObj> splitKeys = autoSplit(operationContext(), 110 /* maxChunkSizeMB */);
    BSONObj expectedSplitPoint = BSON(kPattern << 51);
    ASSERT_EQ(splitKeys.size(), 1);
    ASSERT_BSONOBJ_EQ(splitKeys.at(0), expectedSplitPoint);
}

// Throw exception upon calling autoSplitVector on dropped/unexisting collection
TEST_F(AutoSplitVectorTest, NoCollection) {
    ASSERT_THROWS_CODE(autoSplitVector(operationContext(),
                                       NamespaceString("dummy", "collection"),
                                       BSON(kPattern << 1) /* shard key pattern */,
                                       BSON(kPattern << 0) /* min */,
                                       BSON(kPattern << 100) /* max */,
                                       1 * 1024 * 1024 /* max chunk size in bytes*/),
                       DBException,
                       ErrorCodes::NamespaceNotFound);
}

}  // namespace
}  // namespace mongo
