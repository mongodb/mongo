/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <vector>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/hasher.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/s/resharding/resharding_collection_cloner.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using Doc = Document;
using Arr = std::vector<Value>;
using V = Value;

/**
 * Mock interface to allow specifying mock results for the 'from' collection of the $lookup stage.
 */
class MockMongoInterface final : public StubMongoProcessInterface {
public:
    MockMongoInterface(std::deque<DocumentSource::GetNextResult> mockResults)
        : _mockResults(std::move(mockResults)) {}

    std::unique_ptr<Pipeline, PipelineDeleter> attachCursorSourceToPipeline(
        Pipeline* ownedPipeline, bool allowTargetingShards = true) final {
        std::unique_ptr<Pipeline, PipelineDeleter> pipeline(
            ownedPipeline, PipelineDeleter(ownedPipeline->getContext()->opCtx));

        pipeline->addInitialSource(
            DocumentSourceMock::createForTest(_mockResults, pipeline->getContext()));
        return pipeline;
    }

private:
    std::deque<DocumentSource::GetNextResult> _mockResults;
};

class ReshardingCollectionClonerTest : public ServiceContextTest {
protected:
    std::unique_ptr<Pipeline, PipelineDeleter> makePipeline(
        ShardKeyPattern newShardKeyPattern,
        ShardId recipientShard,
        std::deque<DocumentSource::GetNextResult> sourceCollectionData,
        std::deque<DocumentSource::GetNextResult> configCacheChunksData) {
        auto tempNss = constructTemporaryReshardingNss(_sourceNss.db(), _sourceUUID);
        ReshardingCollectionCloner cloner(std::move(newShardKeyPattern),
                                          _sourceNss,
                                          _sourceUUID,
                                          std::move(recipientShard),
                                          Timestamp(1, 0), /* dummy value */
                                          std::move(tempNss));

        auto pipeline = cloner.makePipeline(
            _opCtx.get(), std::make_shared<MockMongoInterface>(std::move(configCacheChunksData)));

        pipeline->addInitialSource(DocumentSourceMock::createForTest(
            std::move(sourceCollectionData), pipeline->getContext()));

        return pipeline;
    }

    template <class T>
    auto getHashedElementValue(T value) {
        return BSONElementHasher::hash64(BSON("" << value).firstElement(),
                                         BSONElementHasher::DEFAULT_HASH_SEED);
    }

private:
    const NamespaceString _sourceNss = NamespaceString("test"_sd, "collection_being_resharded"_sd);
    const CollectionUUID _sourceUUID = UUID::gen();

    ServiceContext::UniqueOperationContext _opCtx = makeOperationContext();
};

TEST_F(ReshardingCollectionClonerTest, MinKeyChunk) {
    auto pipeline =
        makePipeline(ShardKeyPattern(fromjson("{x: 1}")),
                     ShardId("shard1"),
                     {Doc(fromjson("{_id: 1, x: {$minKey: 1}}")),
                      Doc(fromjson("{_id: 2, x: -0.001}")),
                      Doc(fromjson("{_id: 3, x: NumberLong(0)}")),
                      Doc(fromjson("{_id: 4, x: 0.0}")),
                      Doc(fromjson("{_id: 5, x: 0.001}")),
                      Doc(fromjson("{_id: 6, x: {$maxKey: 1}}"))},
                     {Doc(fromjson("{_id: {x: {$minKey: 1}}, max: {x: 0.0}, shard: 'shard1'}")),
                      Doc(fromjson("{_id: {x: 0.0}, max: {x: {$maxKey: 1}}, shard: 'shard2' }"))});

    auto next = pipeline->getNext();
    ASSERT(next);
    ASSERT_BSONOBJ_BINARY_EQ(BSON("_id" << 1 << "x" << MINKEY), next->toBson());

    next = pipeline->getNext();
    ASSERT(next);
    ASSERT_BSONOBJ_BINARY_EQ(BSON("_id" << 2 << "x" << -0.001), next->toBson());

    ASSERT_FALSE(pipeline->getNext());
}

TEST_F(ReshardingCollectionClonerTest, MaxKeyChunk) {
    auto pipeline =
        makePipeline(ShardKeyPattern(fromjson("{x: 1}")),
                     ShardId("shard2"),
                     {Doc(fromjson("{_id: 1, x: {$minKey: 1}}")),
                      Doc(fromjson("{_id: 2, x: -0.001}")),
                      Doc(fromjson("{_id: 3, x: NumberLong(0)}")),
                      Doc(fromjson("{_id: 4, x: 0.0}")),
                      Doc(fromjson("{_id: 5, x: 0.001}")),
                      Doc(fromjson("{_id: 6, x: {$maxKey: 1}}"))},
                     {Doc(fromjson("{_id: {x: {$minKey: 1}}, max: {x: 0}, shard: 'shard1'}")),
                      Doc(fromjson("{_id: {x: 0}, max: {x: {$maxKey: 1}}, shard: 'shard2' }"))});

    auto next = pipeline->getNext();
    ASSERT(next);
    ASSERT_BSONOBJ_BINARY_EQ(BSON("_id" << 3 << "x" << 0LL), next->toBson());

    next = pipeline->getNext();
    ASSERT(next);
    ASSERT_BSONOBJ_BINARY_EQ(BSON("_id" << 4 << "x" << 0.0), next->toBson());

    next = pipeline->getNext();
    ASSERT(next);
    ASSERT_BSONOBJ_BINARY_EQ(BSON("_id" << 5 << "x" << 0.001), next->toBson());

    next = pipeline->getNext();
    ASSERT(next);
    ASSERT_BSONOBJ_BINARY_EQ(BSON("_id" << 6 << "x" << MAXKEY), next->toBson());

    ASSERT_FALSE(pipeline->getNext());
}

TEST_F(ReshardingCollectionClonerTest, HashedShardKey) {
    auto pipeline = makePipeline(
        ShardKeyPattern(fromjson("{x: 'hashed'}")),
        ShardId("shard2"),
        {Doc(fromjson("{_id: 1, x: {$minKey: 1}}")),
         Doc(fromjson("{_id: 2, x: -1}")),
         Doc(fromjson("{_id: 3, x: -0.123}")),
         Doc(fromjson("{_id: 4, x: 0}")),
         Doc(fromjson("{_id: 5, x: NumberLong(0)}")),
         Doc(fromjson("{_id: 6, x: 0.123}")),
         Doc(fromjson("{_id: 7, x: 1}")),
         Doc(fromjson("{_id: 8, x: {$maxKey: 1}}"))},
        // Documents in a mock config.cache.chunks collection. Mocked collection boundaries:
        // - [MinKey, hash(0))      : shard1
        // - [hash(0), hash(0) + 1) : shard2
        // - [hash(0) + 1, MaxKey]  : shard3
        {Doc{{"_id", Doc{{"x", V(MINKEY)}}},
             {"max", Doc{{"x", getHashedElementValue(0)}}},
             {"shard", "shard1"_sd}},
         Doc{{"_id", Doc{{"x", getHashedElementValue(0)}}},
             {"max", Doc{{"x", getHashedElementValue(0) + 1}}},
             {"shard", "shard2"_sd}},
         Doc{{"_id", Doc{{"x", getHashedElementValue(0) + 1}}},
             {"max", Doc{{"x", V(MAXKEY)}}},
             {"shard", "shard3"_sd}}});

    auto next = pipeline->getNext();
    ASSERT(next);
    ASSERT_BSONOBJ_BINARY_EQ(BSON("_id" << 3 << "x" << -0.123), next->toBson());

    next = pipeline->getNext();
    ASSERT(next);
    ASSERT_BSONOBJ_BINARY_EQ(BSON("_id" << 4 << "x" << 0), next->toBson());

    next = pipeline->getNext();
    ASSERT(next);
    ASSERT_BSONOBJ_BINARY_EQ(BSON("_id" << 5 << "x" << 0LL), next->toBson());

    next = pipeline->getNext();
    ASSERT(next);
    ASSERT_BSONOBJ_BINARY_EQ(BSON("_id" << 6 << "x" << 0.123), next->toBson());

    ASSERT_FALSE(pipeline->getNext());
}

TEST_F(ReshardingCollectionClonerTest, CompoundHashedShardKey) {
    auto pipeline = makePipeline(
        ShardKeyPattern(fromjson("{x: 'hashed', y: 1}")),
        ShardId("shard2"),
        {Doc(fromjson("{_id: 1, x: {$minKey: 1}}")),
         Doc(fromjson("{_id: 2, x: -1}")),
         Doc(fromjson("{_id: 3, x: -0.123, y: -1}")),
         Doc(fromjson("{_id: 4, x: 0, y: 0}")),
         Doc(fromjson("{_id: 5, x: NumberLong(0), y: 1}")),
         Doc(fromjson("{_id: 6, x: 0.123}")),
         Doc(fromjson("{_id: 7, x: 1}")),
         Doc(fromjson("{_id: 8, x: {$maxKey: 1}}"))},
        // Documents in a mock config.cache.chunks collection. Mocked collection boundaries:
        // - [{x: MinKey, y: MinKey}, {x: hash(0), y: 0}) : shard1
        // - [{x: hash(0), y: 0}, {x: hash(0), y: 1})     : shard2
        // - [{x: hash(0), y: 1}, {x: MaxKey, y: MaxKey}] : shard3
        {Doc{{"_id", Doc{{"x", V(MINKEY)}, {"y", V(MINKEY)}}},
             {"max", Doc{{"x", getHashedElementValue(0)}, {"y", 0}}},
             {"shard", "shard1"_sd}},
         Doc{{"_id", Doc{{"x", getHashedElementValue(0)}, {"y", 0}}},
             {"max", Doc{{"x", getHashedElementValue(0)}, {"y", 1}}},
             {"shard", "shard2"_sd}},
         Doc{{"_id", Doc{{"x", getHashedElementValue(0)}, {"y", 1}}},
             {"max", Doc{{"x", V(MAXKEY)}, {"y", V(MAXKEY)}}},
             {"shard", "shard3"_sd}}});

    auto next = pipeline->getNext();
    ASSERT(next);
    ASSERT_BSONOBJ_BINARY_EQ(BSON("_id" << 4 << "x" << 0 << "y" << 0), next->toBson());

    ASSERT_FALSE(pipeline->getNext());
}

}  // namespace
}  // namespace mongo
