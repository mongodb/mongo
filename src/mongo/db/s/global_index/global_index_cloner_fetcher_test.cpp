/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/s/global_index/global_index_cloner_fetcher.h"

#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace global_index {
namespace {

using Doc = Document;

class GlobalIndexClonerFetcherTest : public ShardServerTestFixtureWithCatalogCacheMock {
public:
    void setUp() override {
        ShardServerTestFixtureWithCatalogCacheMock::setUp();
    }

    void tearDown() override {
        ShardServerTestFixtureWithCatalogCacheMock::tearDown();
    }

    ChunkManager createChunkManager(
        const ShardKeyPattern& shardKeyPattern,
        std::deque<DocumentSource::GetNextResult> configCacheChunksData) {
        const OID epoch = OID::gen();
        std::vector<ChunkType> chunks;
        for (const auto& chunkData : configCacheChunksData) {
            const auto bson = chunkData.getDocument().toBson();
            ChunkRange range{bson.getField("_id").Obj().getOwned(),
                             bson.getField("max").Obj().getOwned()};
            ShardId shard{bson.getField("shard").valueStringDataSafe().toString()};
            chunks.emplace_back(_collUUID,
                                std::move(range),
                                ChunkVersion({epoch, Timestamp(1, 1)}, {1, 0}),
                                std::move(shard));
        }

        auto rt = RoutingTableHistory::makeNew(_ns,
                                               _collUUID,
                                               shardKeyPattern.getKeyPattern(),
                                               nullptr,
                                               false,
                                               epoch,
                                               Timestamp(1, 1),
                                               boost::none /* timeseriesFields */,
                                               boost::none /* reshardingFields */,
                                               boost::none /* chunkSizeBytes */,
                                               false,
                                               chunks);

        return ChunkManager(_myShardName,
                            _sourceDbVersion,
                            makeStandaloneRoutingTableHistory(std::move(rt)),
                            boost::none);
    }

    const NamespaceString& ns() const {
        return _ns;
    }

    const UUID& indexUUID() const {
        return _indexUUID;
    }

    const UUID& collUUID() const {
        return _collUUID;
    }

    const ShardId& shardA() const {
        return _shard;
    }

private:
    const NamespaceString _ns{"test", "user"};
    const UUID _indexUUID{UUID::gen()};
    const UUID _collUUID{UUID::gen()};
    const ShardId _shard{"shardA"};
    const DatabaseVersion _sourceDbVersion{UUID::gen(), Timestamp(1, 1)};
};

TEST_F(GlobalIndexClonerFetcherTest, FetcherSortsById) {
    const KeyPattern sourceShardKeyPattern(BSON("sKey" << 1));
    const KeyPattern globalIndexPattern(BSON("uniq" << 1));

    ShardKeyPattern sk{globalIndexPattern};
    std::deque<DocumentSource::GetNextResult> configData{
        Doc(fromjson("{_id: {uniq: {$minKey: 1}}, max: {uniq: 0}, shard: 'myShardName'}")),
        Doc(fromjson("{_id: {uniq: 0}, max: {uniq: {$maxKey: 1}}, shard: 'shardA' }"))};
    getCatalogCacheMock()->setChunkManagerReturnValue(createChunkManager(sk, configData));

    GlobalIndexClonerFetcher fetcher(
        ns(), collUUID(), indexUUID(), shardA(), {}, sourceShardKeyPattern, globalIndexPattern);

    auto pipeline = fetcher.makePipeline(operationContext());

    std::deque<DocumentSource::GetNextResult> mockResults;
    mockResults.emplace_back(Doc(BSON("_id" << 2 << "sKey" << -300 << "uniq" << 30)));
    mockResults.emplace_back(Doc(BSON("_id" << 1 << "sKey" << -100 << "uniq" << 10)));
    mockResults.emplace_back(Doc(BSON("_id" << 3 << "sKey" << -200 << "uniq" << 20)));

    auto mockSource =
        DocumentSourceMock::createForTest(std::move(mockResults), pipeline->getContext());
    pipeline->addInitialSource(mockSource);

    auto next = pipeline->getNext();
    ASSERT_TRUE(next);
    ASSERT_BSONOBJ_EQ(fromjson("{_id: {uniq: 10}, documentKey: {_id: 1, sKey: -100}}"),
                      next->toBson());

    next = pipeline->getNext();
    ASSERT_TRUE(next);
    ASSERT_BSONOBJ_EQ(fromjson("{_id: {uniq: 30}, documentKey: {_id: 2, sKey: -300}}"),
                      next->toBson());

    next = pipeline->getNext();
    ASSERT_TRUE(next);
    ASSERT_BSONOBJ_EQ(fromjson("{_id: {uniq: 20}, documentKey: {_id: 3, sKey: -200}}"),
                      next->toBson());

    ASSERT_FALSE(pipeline->getNext());
}

TEST_F(GlobalIndexClonerFetcherTest, DocsThatDontBelongToThisShardAreFileteredOut) {
    const KeyPattern sourceShardKeyPattern(BSON("sKey" << 1));
    const KeyPattern globalIndexPattern(BSON("uniq" << 1));

    ShardKeyPattern sk{globalIndexPattern};
    std::deque<DocumentSource::GetNextResult> configData{
        Doc(fromjson("{_id: {uniq: {$minKey: 1}}, max: {uniq: 0}, shard: 'myShardName'}")),
        Doc(fromjson("{_id: {uniq: 0}, max: {uniq: {$maxKey: 1}}, shard: 'shardA' }"))};
    getCatalogCacheMock()->setChunkManagerReturnValue(createChunkManager(sk, configData));

    GlobalIndexClonerFetcher fetcher(
        ns(), collUUID(), indexUUID(), shardA(), {}, sourceShardKeyPattern, globalIndexPattern);

    auto pipeline = fetcher.makePipeline(operationContext());

    std::deque<DocumentSource::GetNextResult> mockResults;
    mockResults.emplace_back(Doc(BSON("_id" << 3 << "sKey" << -300 << "uniq" << 30)));
    mockResults.emplace_back(Doc(BSON("_id" << 1 << "sKey" << -100 << "uniq" << 10)));
    mockResults.emplace_back(Doc(BSON("_id" << 2 << "sKey" << -200 << "uniq" << -20)));

    auto mockSource =
        DocumentSourceMock::createForTest(std::move(mockResults), pipeline->getContext());
    pipeline->addInitialSource(mockSource);

    auto next = pipeline->getNext();
    ASSERT_TRUE(next);
    ASSERT_BSONOBJ_EQ(fromjson("{_id: {uniq: 10}, documentKey: {_id: 1, sKey: -100}}"),
                      next->toBson());

    next = pipeline->getNext();
    ASSERT_TRUE(next);
    ASSERT_BSONOBJ_EQ(fromjson("{_id: {uniq: 30}, documentKey: {_id: 3, sKey: -300}}"),
                      next->toBson());

    ASSERT_FALSE(pipeline->getNext());
}

TEST_F(GlobalIndexClonerFetcherTest, CorrectlyTransformCompoundKeyPatterns) {
    const KeyPattern sourceShardKeyPattern(BSON("sKeyA" << 1 << "sKeyB" << 1));
    const KeyPattern globalIndexPattern(BSON("uniqA" << 1 << "uniqB" << 1));

    ShardKeyPattern sk{globalIndexPattern};
    std::deque<DocumentSource::GetNextResult> configData{
        Doc(fromjson("{_id: {uniqA: {$minKey: 1}, uniqB: {$minKey: 1}}, max: {uniqA: 0, uniqB: 0}, "
                     "shard: 'myShardName'}")),
        Doc(fromjson("{_id: {uniqA: 0, uniqB: 0}, max: {uniqA: {$maxKey: 1}, uniqB: {$maxKey: 1}}, "
                     "shard: 'shardA' }"))};
    getCatalogCacheMock()->setChunkManagerReturnValue(createChunkManager(sk, configData));

    GlobalIndexClonerFetcher fetcher(
        ns(), collUUID(), indexUUID(), shardA(), {}, sourceShardKeyPattern, globalIndexPattern);

    auto pipeline = fetcher.makePipeline(operationContext());

    std::deque<DocumentSource::GetNextResult> mockResults;
    mockResults.emplace_back(Doc(BSON("_id" << 3 << "sKeyA" << -300 << "sKeyB"
                                            << "3"
                                            << "uniqA" << 30 << "uniqB"
                                            << "30")));
    mockResults.emplace_back(Doc(BSON("_id" << 1 << "sKeyA" << -100 << "sKeyB"
                                            << "1"
                                            << "uniqA" << 10 << "uniqB"
                                            << "10")));
    mockResults.emplace_back(Doc(BSON("_id" << 2 << "sKeyA" << -200 << "sKeyB"
                                            << "2"
                                            << "uniqA" << 20 << "uniqB"
                                            << "20")));

    auto mockSource =
        DocumentSourceMock::createForTest(std::move(mockResults), pipeline->getContext());
    pipeline->addInitialSource(mockSource);

    auto next = pipeline->getNext();
    ASSERT_TRUE(next);
    ASSERT_BSONOBJ_EQ(
        fromjson("{_id: {uniqA: 10, uniqB: '10'}, documentKey: {_id: 1, sKeyA: -100, sKeyB: '1'}}"),
        next->toBson());

    next = pipeline->getNext();
    ASSERT_TRUE(next);
    ASSERT_BSONOBJ_EQ(
        fromjson("{_id: {uniqA: 20, uniqB: '20'}, documentKey: {_id: 2, sKeyA: -200, sKeyB: '2'}}"),
        next->toBson());

    next = pipeline->getNext();
    ASSERT_TRUE(next);
    ASSERT_BSONOBJ_EQ(
        fromjson("{_id: {uniqA: 30, uniqB: '30'}, documentKey: {_id: 3, sKeyA: -300, sKeyB: '3'}}"),
        next->toBson());

    ASSERT_FALSE(pipeline->getNext());
}

TEST_F(GlobalIndexClonerFetcherTest, IdGlobalIdxPattern) {
    const KeyPattern sourceShardKeyPattern(BSON("sKey" << 1));
    const KeyPattern globalIndexPattern(BSON("_id" << 1));

    ShardKeyPattern sk{globalIndexPattern};
    std::deque<DocumentSource::GetNextResult> configData{
        Doc(fromjson("{_id: {_id: {$minKey: 1}}, max: {_id: 0}, shard: 'myShardName'}")),
        Doc(fromjson("{_id: {_id: 0}, max: {_id: {$maxKey: 1}}, shard: 'shardA' }"))};
    getCatalogCacheMock()->setChunkManagerReturnValue(createChunkManager(sk, configData));

    GlobalIndexClonerFetcher fetcher(
        ns(), collUUID(), indexUUID(), shardA(), {}, sourceShardKeyPattern, globalIndexPattern);

    auto pipeline = fetcher.makePipeline(operationContext());

    std::deque<DocumentSource::GetNextResult> mockResults;
    mockResults.emplace_back(Doc(BSON("_id" << 3 << "sKey" << -300)));
    mockResults.emplace_back(Doc(BSON("_id" << 1 << "sKey" << -100)));

    auto mockSource =
        DocumentSourceMock::createForTest(std::move(mockResults), pipeline->getContext());
    pipeline->addInitialSource(mockSource);

    auto next = pipeline->getNext();
    ASSERT_TRUE(next);
    ASSERT_BSONOBJ_EQ(fromjson("{_id: {_id: 1}, documentKey: {_id: 1, sKey: -100}}"),
                      next->toBson());

    next = pipeline->getNext();
    ASSERT_TRUE(next);
    ASSERT_BSONOBJ_EQ(fromjson("{_id: {_id: 3}, documentKey: {_id: 3, sKey: -300}}"),
                      next->toBson());

    ASSERT_FALSE(pipeline->getNext());
}

TEST_F(GlobalIndexClonerFetcherTest, ShardKeyPatternHasID) {
    const KeyPattern sourceShardKeyPattern(BSON("_id" << 1));
    const KeyPattern globalIndexPattern(BSON("uniq" << 1));

    ShardKeyPattern sk{globalIndexPattern};
    std::deque<DocumentSource::GetNextResult> configData{
        Doc(fromjson("{_id: {uniq: {$minKey: 1}}, max: {uniq: 0}, shard: 'myShardName'}")),
        Doc(fromjson("{_id: {uniq: 0}, max: {uniq: {$maxKey: 1}}, shard: 'shardA' }"))};
    getCatalogCacheMock()->setChunkManagerReturnValue(createChunkManager(sk, configData));

    GlobalIndexClonerFetcher fetcher(
        ns(), collUUID(), indexUUID(), shardA(), {}, sourceShardKeyPattern, globalIndexPattern);

    auto pipeline = fetcher.makePipeline(operationContext());

    std::deque<DocumentSource::GetNextResult> mockResults;
    mockResults.emplace_back(Doc(BSON("_id" << 3 << "uniq" << 300)));
    mockResults.emplace_back(Doc(BSON("_id" << 1 << "uniq" << 100)));

    auto mockSource =
        DocumentSourceMock::createForTest(std::move(mockResults), pipeline->getContext());
    pipeline->addInitialSource(mockSource);

    auto next = pipeline->getNext();
    ASSERT_TRUE(next);
    ASSERT_BSONOBJ_EQ(BSON("_id" << BSON("uniq" << 100) << "documentKey" << BSON("_id" << 1)),
                      next->toBson());

    next = pipeline->getNext();
    ASSERT_TRUE(next);
    ASSERT_BSONOBJ_EQ(BSON("_id" << BSON("uniq" << 300) << "documentKey" << BSON("_id" << 3)),
                      next->toBson());

    ASSERT_FALSE(pipeline->getNext());
}

TEST_F(GlobalIndexClonerFetcherTest, DocsWithIncompleteFieldsCanStillBeFetched) {
    const KeyPattern sourceShardKeyPattern(BSON("sKey" << 1));
    const KeyPattern globalIndexPattern(BSON("a" << 1 << "b" << 1));

    ShardKeyPattern sk{globalIndexPattern};
    std::deque<DocumentSource::GetNextResult> configData{
        Doc(fromjson("{_id: {a: {$minKey: 1}, b: {$minKey: 1}}, max: {a: {$maxKey: 1}, b: "
                     "{$maxKey: 1}}, shard: 'shardA'}"))};
    getCatalogCacheMock()->setChunkManagerReturnValue(createChunkManager(sk, configData));

    GlobalIndexClonerFetcher fetcher(
        ns(), collUUID(), indexUUID(), shardA(), {}, sourceShardKeyPattern, globalIndexPattern);

    auto pipeline = fetcher.makePipeline(operationContext());

    std::deque<DocumentSource::GetNextResult> mockResults;
    mockResults.emplace_back(Doc(BSON("_id" << 2 << "sKey" << 200 << "a" << 20)));
    mockResults.emplace_back(Doc(BSON("_id" << 1 << "sKey" << 100 << "b" << 10)));
    mockResults.emplace_back(Doc(BSON("_id" << 3 << "sKey" << 300 << "c" << 30)));

    auto mockSource =
        DocumentSourceMock::createForTest(std::move(mockResults), pipeline->getContext());
    pipeline->addInitialSource(mockSource);

    auto next = pipeline->getNext();
    ASSERT_TRUE(next);
    ASSERT_BSONOBJ_EQ(fromjson("{_id: {b: 10}, documentKey: {_id: 1, sKey: 100}}"), next->toBson());

    next = pipeline->getNext();
    ASSERT_TRUE(next);
    ASSERT_BSONOBJ_EQ(fromjson("{_id: {a: 20}, documentKey: {_id: 2, sKey: 200}}"), next->toBson());


    next = pipeline->getNext();
    ASSERT_TRUE(next);
    ASSERT_BSONOBJ_EQ(fromjson("{_id: {}, documentKey: {_id: 3, sKey: 300}}"), next->toBson());

    ASSERT_FALSE(pipeline->getNext());
}

TEST_F(GlobalIndexClonerFetcherTest, CanFetchDocumentsWithIncompleteShardKey) {
    const KeyPattern sourceShardKeyPattern(BSON("sKeyA" << 1 << "sKeyB" << 1));
    const KeyPattern globalIndexPattern(BSON("uniq" << 1));

    ShardKeyPattern sk{globalIndexPattern};
    std::deque<DocumentSource::GetNextResult> configData{
        Doc(fromjson("{_id: {uniq: {$minKey: 1}}, max: {uniq: {$maxKey: 1}}, "
                     "shard: 'shardA'}"))};
    getCatalogCacheMock()->setChunkManagerReturnValue(createChunkManager(sk, configData));

    GlobalIndexClonerFetcher fetcher(
        ns(), collUUID(), indexUUID(), shardA(), {}, sourceShardKeyPattern, globalIndexPattern);

    auto pipeline = fetcher.makePipeline(operationContext());

    std::deque<DocumentSource::GetNextResult> mockResults;
    mockResults.emplace_back(Doc(BSON("_id" << 1 << "sKeyA" << 1 << "uniq" << 10)));
    mockResults.emplace_back(Doc(BSON("_id" << 2 << "sKeyB" << 2 << "uniq" << 20)));

    auto mockSource =
        DocumentSourceMock::createForTest(std::move(mockResults), pipeline->getContext());
    pipeline->addInitialSource(mockSource);

    auto next = pipeline->getNext();
    ASSERT_TRUE(next);
    ASSERT_BSONOBJ_EQ(fromjson("{_id: {uniq: 10}, documentKey: {_id: 1, sKeyA: 1, sKeyB: null}}"),
                      next->toBson());

    next = pipeline->getNext();
    ASSERT_TRUE(next);
    ASSERT_BSONOBJ_EQ(fromjson("{_id: {uniq: 20}, documentKey: {_id: 2, sKeyA: null, sKeyB: 2}}"),
                      next->toBson());

    ASSERT_FALSE(pipeline->getNext());
}

TEST_F(GlobalIndexClonerFetcherTest, CanFetchDocumentsWithDottedShardKey) {
    const KeyPattern sourceShardKeyPattern(BSON("x.a" << 1));
    const KeyPattern globalIndexPattern(BSON("uniq" << 1));

    ShardKeyPattern sk{globalIndexPattern};
    std::deque<DocumentSource::GetNextResult> configData{
        Doc(fromjson("{_id: {uniq: {$minKey: 1}}, max: {uniq: {$maxKey: 1}}, "
                     "shard: 'shardA'}"))};
    getCatalogCacheMock()->setChunkManagerReturnValue(createChunkManager(sk, configData));

    GlobalIndexClonerFetcher fetcher(
        ns(), collUUID(), indexUUID(), shardA(), {}, sourceShardKeyPattern, globalIndexPattern);

    auto pipeline = fetcher.makePipeline(operationContext());

    std::deque<DocumentSource::GetNextResult> mockResults;
    mockResults.emplace_back(Doc(BSON("_id" << 1 << "x" << BSON("a" << -1) << "uniq" << 10)));
    mockResults.emplace_back(Doc(BSON("_id" << 2 << "x" << BSON("a" << -2) << "uniq" << 20)));

    auto mockSource =
        DocumentSourceMock::createForTest(std::move(mockResults), pipeline->getContext());
    pipeline->addInitialSource(mockSource);

    auto next = pipeline->getNext();
    ASSERT_TRUE(next);
    ASSERT_BSONOBJ_EQ(fromjson("{_id: {uniq: 10}, documentKey: {_id: 1, 'x.a': -1}}"),
                      next->toBson());

    next = pipeline->getNext();
    ASSERT_TRUE(next);
    ASSERT_BSONOBJ_EQ(fromjson("{_id: {uniq: 20}, documentKey: {_id: 2, 'x.a': -2}}"),
                      next->toBson());

    ASSERT_FALSE(pipeline->getNext());
}

TEST_F(GlobalIndexClonerFetcherTest, CanFetchDocumentsWithDottedIdInShardKey) {
    const KeyPattern sourceShardKeyPattern(BSON("_id.a" << 1 << "uniq" << 1));
    const KeyPattern globalIndexPattern(BSON("uniq" << 1));

    ShardKeyPattern sk{globalIndexPattern};
    std::deque<DocumentSource::GetNextResult> configData{
        Doc(fromjson("{_id: {uniq: {$minKey: 1}}, max: {uniq: {$maxKey: 1}}, "
                     "shard: 'shardA'}"))};
    getCatalogCacheMock()->setChunkManagerReturnValue(createChunkManager(sk, configData));

    GlobalIndexClonerFetcher fetcher(
        ns(), collUUID(), indexUUID(), shardA(), {}, sourceShardKeyPattern, globalIndexPattern);

    auto pipeline = fetcher.makePipeline(operationContext());

    std::deque<DocumentSource::GetNextResult> mockResults;
    mockResults.emplace_back(Doc(BSON("_id" << BSON("a" << 1 << "b" << -1) << "uniq" << 10)));
    mockResults.emplace_back(Doc(BSON("_id" << BSON("a" << 2 << "b" << -2) << "uniq" << 20)));

    auto mockSource =
        DocumentSourceMock::createForTest(std::move(mockResults), pipeline->getContext());
    pipeline->addInitialSource(mockSource);

    auto next = pipeline->getNext();
    ASSERT_TRUE(next);
    ASSERT_BSONOBJ_EQ(
        fromjson("{_id: {uniq: 10}, documentKey: {_id: {a: 1, b: -1}, '_id.a': 1, uniq: 10}}"),
        next->toBson());

    next = pipeline->getNext();
    ASSERT_TRUE(next);
    ASSERT_BSONOBJ_EQ(
        fromjson("{_id: {uniq: 20}, documentKey: {_id: {a: 2, b: -2}, '_id.a': 2, uniq: 20}}"),
        next->toBson());

    ASSERT_FALSE(pipeline->getNext());
}

TEST_F(GlobalIndexClonerFetcherTest, FetcherShouldSkipDocsIfResumeIdIsSet) {
    const KeyPattern sourceShardKeyPattern(BSON("sKey" << 1));
    const KeyPattern globalIndexPattern(BSON("uniq" << 1));

    ShardKeyPattern sk{globalIndexPattern};
    std::deque<DocumentSource::GetNextResult> configData{
        Doc(fromjson("{_id: {uniq: {$minKey: 1}}, max: {uniq: {$maxKey: 1}}, shard: 'shardA'}"))};
    getCatalogCacheMock()->setChunkManagerReturnValue(createChunkManager(sk, configData));

    GlobalIndexClonerFetcher fetcher(
        ns(), collUUID(), indexUUID(), shardA(), {}, sourceShardKeyPattern, globalIndexPattern);

    std::deque<DocumentSource::GetNextResult> mockResults;
    const auto kRequiresEscapingBSON = BSON("$ne" << 4);
    mockResults.emplace_back(Doc(BSON("_id" << 1 << "sKey" << 1 << "uniq" << 1)));
    mockResults.emplace_back(Doc(BSON("_id" << 2 << "sKey" << 2 << "uniq" << 2)));
    mockResults.emplace_back(Doc(BSON("_id"
                                      << "3"
                                      << "sKey" << 3 << "uniq" << 3)));
    mockResults.emplace_back(
        Doc(BSON("_id" << kRequiresEscapingBSON << "sKey" << 4 << "uniq" << 4)));

    fetcher.setResumeId(Value(kRequiresEscapingBSON));
    auto pipeline = fetcher.makePipeline(operationContext());

    {
        auto mockSource = DocumentSourceMock::createForTest(mockResults, pipeline->getContext());
        pipeline->addInitialSource(mockSource);

        auto next = pipeline->getNext();
        ASSERT_TRUE(next);
        ASSERT_BSONOBJ_EQ(fromjson("{_id: {uniq: 4}, documentKey: {_id: {$ne: 4}, sKey: 4}}"),
                          next->toBson());

        ASSERT_FALSE(pipeline->getNext());
    }

    // Test that resumeId can be set and overrides old setting.

    fetcher.setResumeId(Value(2));
    pipeline = fetcher.makePipeline(operationContext());

    {
        auto mockSource = DocumentSourceMock::createForTest(mockResults, pipeline->getContext());
        pipeline->addInitialSource(mockSource);

        auto next = pipeline->getNext();
        ASSERT_TRUE(next);
        ASSERT_BSONOBJ_EQ(fromjson("{_id: {uniq: 2}, documentKey: {_id: 2, sKey: 2}}"),
                          next->toBson());

        next = pipeline->getNext();
        ASSERT_TRUE(next);
        ASSERT_BSONOBJ_EQ(fromjson("{_id: {uniq: 3}, documentKey: {_id: \"3\", sKey: 3}}"),
                          next->toBson());

        next = pipeline->getNext();
        ASSERT_TRUE(next);
        ASSERT_BSONOBJ_EQ(fromjson("{_id: {uniq: 4}, documentKey: {_id: {$ne: 4}, sKey: 4}}"),
                          next->toBson());


        ASSERT_FALSE(pipeline->getNext());
    }
}

}  // namespace
}  // namespace global_index
}  // namespace mongo
