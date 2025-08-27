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

#include "mongo/db/s/resharding/resharding_collection_cloner.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/agg/pipeline_builder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache_mock.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection_common_types_gen.h"
#include "mongo/db/hasher.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/create_collection.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_catalog/operation_sharding_state.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/pipeline/sharded_agg_helpers_targeting_policy.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/s/resharding/recipient_resume_document_gen.h"
#include "mongo/db/s/resharding/resharding_data_copy_util.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/intrusive_counter.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

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

    std::unique_ptr<Pipeline> preparePipelineForExecution(
        Pipeline* ownedPipeline,
        ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed,
        boost::optional<BSONObj> readConcern = boost::none) final {
        std::unique_ptr<Pipeline> pipeline(ownedPipeline);

        pipeline->addInitialSource(
            DocumentSourceMock::createForTest(_mockResults, pipeline->getContext()));
        return pipeline;
    }

    std::unique_ptr<Pipeline> preparePipelineForExecution(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const AggregateCommandRequest& aggRequest,
        Pipeline* pipeline,
        boost::optional<BSONObj> shardCursorsSortSpec = boost::none,
        ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed,
        boost::optional<BSONObj> readConcern = boost::none,
        bool shouldUseCollectionDefaultCollator = false) final {
        return preparePipelineForExecution(pipeline, shardTargetingPolicy, std::move(readConcern));
    }

private:
    std::deque<DocumentSource::GetNextResult> _mockResults;
};

struct TestOptions {
    bool storeProgress;

    BSONObj toBSON() const {
        BSONObjBuilder bob;
        bob.append("storeProgress", storeProgress);
        return bob.obj();
    }
};

std::vector<TestOptions> makeAllTestOptions() {
    std::vector<TestOptions> testOptions;
    for (bool storeProgress : {false, true}) {
        testOptions.push_back({storeProgress});
    }
    return testOptions;
}

class ReshardingCollectionClonerTest : public ShardServerTestFixtureWithCatalogCacheMock {
protected:
    void initializePipelineTest(
        const ShardKeyPattern& newShardKeyPattern,
        const ShardId& recipientShard,
        const std::deque<DocumentSource::GetNextResult>& sourceCollectionData,
        const std::deque<DocumentSource::GetNextResult>& configCacheChunksData,
        const TestOptions& testOptions) {
        _sourceNss = NamespaceString::createNamespaceString_forTest(
            "testDb"_sd + std::to_string(_sourceDbNum++), "testColl"_sd);
        _sourceUUID = UUID::gen();
        _tempNss = resharding::constructTemporaryReshardingNss(_sourceNss, _sourceUUID);
        _reshardingUUID = UUID::gen();

        createTestCollection(operationContext(), _tempNss);

        _metrics = ReshardingMetrics::makeInstance_forTest(
            _sourceUUID,
            newShardKeyPattern.toBSON(),
            _sourceNss,
            ReshardingMetrics::Role::kRecipient,
            getServiceContext()->getFastClockSource()->now(),
            getServiceContext());

        _cloner = std::make_unique<ReshardingCollectionCloner>(
            _metrics.get(),
            _reshardingUUID,
            ShardKeyPattern(newShardKeyPattern.toBSON()),
            _sourceNss,
            _sourceUUID,
            recipientShard,
            Timestamp(1, 0), /* dummy value */
            _tempNss,
            testOptions.storeProgress,
            false);

        getCatalogCacheMock()->setCollectionReturnValue(
            _tempNss,
            CollectionRoutingInfo(createChunkManager(newShardKeyPattern, configCacheChunksData),
                                  DatabaseTypeValueHandle(DatabaseType{_tempNss.dbName(),
                                                                       getSourceId().getShardId(),
                                                                       _sourceDbVersion})));

        auto [rawPipeline, expCtx] = _cloner->makeRawNaturalOrderPipeline(
            operationContext(), std::make_shared<MockMongoInterface>(configCacheChunksData));
        _pipeline = Pipeline::parse(rawPipeline, expCtx);

        _pipeline->addInitialSource(
            DocumentSourceMock::createForTest(sourceCollectionData, _pipeline->getContext()));
        _execPipeline = exec::agg::buildPipeline(_pipeline->freeze());
        _resumeTokenNum = 0;
    }

    template <class T>
    auto getHashedElementValue(T value) {
        return BSONElementHasher::hash64(BSON("" << value).firstElement(),
                                         BSONElementHasher::DEFAULT_HASH_SEED);
    }

    void setUp() override {
        ShardServerTestFixtureWithCatalogCacheMock::setUp();

        uassertStatusOK(createCollection(
            operationContext(),
            NamespaceString::kRecipientReshardingResumeDataNamespace.dbName(),
            BSON("create" << NamespaceString::kRecipientReshardingResumeDataNamespace.coll())));
        uassertStatusOK(createCollection(
            operationContext(),
            NamespaceString::kSessionTransactionsTableNamespace.dbName(),
            BSON("create" << NamespaceString::kSessionTransactionsTableNamespace.coll())));
        DBDirectClient client(operationContext());
        client.createIndexes(NamespaceString::kSessionTransactionsTableNamespace,
                             {MongoDSessionCatalog::getConfigTxnPartialIndexSpec()});
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
            ShardId shard{std::string{bson.getField("shard").valueStringDataSafe()}};
            chunks.emplace_back(_sourceUUID,
                                std::move(range),
                                ChunkVersion({epoch, Timestamp(1, 1)}, {1, 0}),
                                std::move(shard));
        }

        auto rt = RoutingTableHistory::makeNew(_sourceNss,
                                               _sourceUUID,
                                               shardKeyPattern.getKeyPattern(),
                                               false, /* unsplittable */
                                               nullptr,
                                               false,
                                               epoch,
                                               Timestamp(1, 1),
                                               boost::none /* timeseriesFields */,
                                               boost::none /* reshardingFields */,
                                               false,
                                               chunks);

        return ChunkManager(makeStandaloneRoutingTableHistory(std::move(rt)), boost::none);
    }

    /**
     * Fetches and inserts a single batch of documents. Returns true if there are more documents to
     * be fetched and inserted, and returns false otherwise.
     */
    bool doOneBatch(OperationContext* opCtx,
                    Pipeline& pipeline,
                    exec::agg::Pipeline& execPipeline,
                    TxnNumber& txnNum,
                    ShardId donorShard,
                    HostAndPort donorHost,
                    size_t batchSize) {
        execPipeline.reattachToOperationContext(opCtx);
        pipeline.reattachToOperationContext(opCtx);
        ON_BLOCK_EXIT([&pipeline, &execPipeline] {
            execPipeline.detachFromOperationContext();
            pipeline.detachFromOperationContext();
        });
        _advanceResumeToken(batchSize);

        std::vector<InsertStatement> batch;
        do {
            auto doc = execPipeline.getNext();
            if (!doc) {
                break;
            }

            auto obj = doc->toBson();
            batch.emplace_back(obj.getOwned());
        } while (batch.size() < batchSize);

        if (batch.empty()) {
            return false;
        }

        _cloner->writeOneBatch(opCtx, txnNum, batch, donorShard, donorHost, _getResumeToken());
        return true;
    }

    void runPipelineTest(
        ShardKeyPattern shardKey,
        const ShardId& recipientShard,
        std::deque<DocumentSource::GetNextResult> collectionData,
        std::deque<DocumentSource::GetNextResult> configData,
        const TestOptions& testOptions,
        int64_t expectedDocumentsCount,
        std::function<void(std::unique_ptr<SeekableRecordCursor>)> verifyFunction) {
        initializePipelineTest(shardKey, recipientShard, collectionData, configData, testOptions);

        auto newClient =
            operationContext()->getServiceContext()->getService()->makeClient("AlternativeClient");
        AlternativeClientRegion acr(newClient);
        auto opCtxHolder = cc().makeOperationContext();
        auto opCtx = opCtxHolder.get();

        opCtx->setLogicalSessionId(makeLogicalSessionId(opCtx));

        TxnNumber txnNum(0);
        // Make the documents require multiple insert batches.
        const size_t batchSize = std::max(static_cast<int64_t>(1), expectedDocumentsCount / 2);

        while (doOneBatch(
            opCtx, *_pipeline, *_execPipeline, txnNum, kMyShardName, _myHostAndPort, batchSize)) {
            const auto tempColl =
                acquireCollection(opCtx,
                                  CollectionAcquisitionRequest::fromOpCtx(
                                      opCtx, _tempNss, AcquisitionPrerequisites::kRead),
                                  MODE_IX);
            ASSERT_EQ(tempColl.getCollectionPtr()->numRecords(opCtx),
                      _metrics->getDocumentsProcessedCount());
            ASSERT_EQ(tempColl.getCollectionPtr()->dataSize(opCtx),
                      _metrics->getBytesWrittenCount());

            auto doc = getResumeDataDocument(opCtx);
            auto parsedDoc = ReshardingRecipientResumeData::parse(
                doc, IDLParserContext("ReshardingCollectionClonerTest"));
            ASSERT_BSONOBJ_EQ(parsedDoc.getId().toBSON(), getSourceId().toBSON());
            ASSERT_EQ(parsedDoc.getDonorHost(), _myHostAndPort);
            ASSERT_BSONOBJ_EQ(*parsedDoc.getResumeToken(), _getResumeToken());

            if (testOptions.storeProgress) {
                ASSERT_EQ(tempColl.getCollectionPtr()->numRecords(opCtx),
                          *parsedDoc.getDocumentsCopied());
                ASSERT_GT(tempColl.getCollectionPtr()->dataSize(opCtx), 0);
                ASSERT_EQ(tempColl.getCollectionPtr()->dataSize(opCtx),
                          *parsedDoc.getBytesCopied());
            } else {
                ASSERT(!parsedDoc.getDocumentsCopied());
            }
        }

        const auto tempColl =
            acquireCollection(opCtx,
                              CollectionAcquisitionRequest::fromOpCtx(
                                  opCtx, _tempNss, AcquisitionPrerequisites::kRead),
                              MODE_IX);
        ASSERT_EQ(tempColl.getCollectionPtr()->numRecords(operationContext()),
                  expectedDocumentsCount);
        ASSERT_EQ(_metrics->getDocumentsProcessedCount(), expectedDocumentsCount);
        ASSERT_GT(tempColl.getCollectionPtr()->dataSize(opCtx), 0);
        ASSERT_EQ(tempColl.getCollectionPtr()->dataSize(opCtx), _metrics->getBytesWrittenCount());
        verifyFunction(tempColl.getCollectionPtr()->getCursor(opCtx));
    }

    ReshardingSourceId getSourceId() const {
        return {_reshardingUUID, kMyShardName};
    }

    BSONObj getResumeDataDocument(OperationContext* opCtx) const {
        DBDirectClient client(opCtx);
        FindCommandRequest request(NamespaceString::kRecipientReshardingResumeDataNamespace);
        return client.findOne(
            NamespaceString::kRecipientReshardingResumeDataNamespace,
            BSON(ReshardingRecipientResumeData::kIdFieldName << getSourceId().toBSON()));
    }

private:
    void _advanceResumeToken(size_t batchSize) {
        _resumeTokenNum += batchSize;
    }

    BSONObj _getResumeToken() {
        return BSON("tokenNum" << _resumeTokenNum);
    }

    const DatabaseVersion _sourceDbVersion{UUID::gen(), Timestamp(1, 1)};
    int _sourceDbNum;

    const HostAndPort _myHostAndPort{kMyShardName + ":123"};


    // Initialized at the start of each test case.
    NamespaceString _sourceNss;
    UUID _sourceUUID = UUID::gen();
    NamespaceString _tempNss;
    UUID _reshardingUUID = UUID::gen();
    std::unique_ptr<ReshardingMetrics> _metrics;
    std::unique_ptr<ReshardingCollectionCloner> _cloner;
    std::unique_ptr<Pipeline> _pipeline;
    std::unique_ptr<exec::agg::Pipeline> _execPipeline;
    // Mock 'postBatchResumeToken'. Incremented every a mock batch is processed.
    int _resumeTokenNum = 0;
};

TEST_F(ReshardingCollectionClonerTest, MinKeyChunk) {
    for (const auto& testOptions : makeAllTestOptions()) {
        ShardKeyPattern sk{fromjson("{x: 1}")};
        std::deque<DocumentSource::GetNextResult> collectionData{
            Doc(fromjson("{_id: 1, x: {$minKey: 1}}")),
            Doc(fromjson("{_id: 2, x: -0.001}")),
            Doc(fromjson("{_id: 3, x: NumberLong(0)}")),
            Doc(fromjson("{_id: 4, x: 0.0}")),
            Doc(fromjson("{_id: 5, x: 0.001}")),
            Doc(fromjson("{_id: 6, x: {$maxKey: 1}}"))};
        std::deque<DocumentSource::GetNextResult> configData{
            Doc(fromjson("{_id: {x: {$minKey: 1}}, max: {x: 0.0}, shard: 'myShardName'}")),
            Doc(fromjson("{_id: {x: 0.0}, max: {x: {$maxKey: 1}}, shard: 'shard2' }"))};
        constexpr auto kExpectedCopiedCount = 2;
        const auto verify = [](auto cursor) {
            auto next = cursor->next();
            ASSERT(next);
            ASSERT_BSONOBJ_BINARY_EQ(BSON("_id" << 1 << "x" << MINKEY), next->data.toBson());

            next = cursor->next();
            ASSERT(next);
            ASSERT_BSONOBJ_BINARY_EQ(BSON("_id" << 2 << "x" << -0.001), next->data.toBson());

            ASSERT_FALSE(cursor->next());
        };

        runPipelineTest(std::move(sk),
                        kMyShardName,
                        std::move(collectionData),
                        std::move(configData),
                        testOptions,
                        kExpectedCopiedCount,
                        verify);
    }
}

TEST_F(ReshardingCollectionClonerTest, MaxKeyChunk) {
    for (const auto& testOptions : makeAllTestOptions()) {
        ShardKeyPattern sk{fromjson("{x: 1}")};
        std::deque<DocumentSource::GetNextResult> collectionData{
            Doc(fromjson("{_id: 1, x: {$minKey: 1}}")),
            Doc(fromjson("{_id: 2, x: -0.001}")),
            Doc(fromjson("{_id: 3, x: NumberLong(0)}")),
            Doc(fromjson("{_id: 4, x: 0.0}")),
            Doc(fromjson("{_id: 5, x: 0.001}")),
            Doc(fromjson("{_id: 6, x: {$maxKey: 1}}"))};
        std::deque<DocumentSource::GetNextResult> configData{
            Doc(fromjson("{_id: {x: {$minKey: 1}}, max: {x: 0.0}, shard: 'myShardName'}")),
            Doc(fromjson("{_id: {x: 0.0}, max: {x: {$maxKey: 1}}, shard: 'shard2' }")),
        };
        constexpr auto kExpectedCopiedCount = 4;
        const auto verify = [](auto cursor) {
            auto next = cursor->next();
            ASSERT(next);
            ASSERT_BSONOBJ_BINARY_EQ(BSON("_id" << 3 << "x" << 0LL), next->data.toBson());

            next = cursor->next();
            ASSERT(next);
            ASSERT_BSONOBJ_BINARY_EQ(BSON("_id" << 4 << "x" << 0.0), next->data.toBson());

            next = cursor->next();
            ASSERT(next);
            ASSERT_BSONOBJ_BINARY_EQ(BSON("_id" << 5 << "x" << 0.001), next->data.toBson());

            next = cursor->next();
            ASSERT(next);
            ASSERT_BSONOBJ_BINARY_EQ(BSON("_id" << 6 << "x" << MAXKEY), next->data.toBson());

            ASSERT_FALSE(cursor->next());
        };

        runPipelineTest(std::move(sk),
                        ShardId("shard2"),
                        std::move(collectionData),
                        std::move(configData),
                        testOptions,
                        kExpectedCopiedCount,
                        verify);
    }
}

TEST_F(ReshardingCollectionClonerTest, HashedShardKey) {
    for (const auto& testOptions : makeAllTestOptions()) {
        ShardKeyPattern sk{fromjson("{x: 'hashed'}")};
        std::deque<DocumentSource::GetNextResult> collectionData{
            Doc(fromjson("{_id: 1, x: {$minKey: 1}}")),
            Doc(fromjson("{_id: 2, x: -1}")),
            Doc(fromjson("{_id: 3, x: -0.123}")),
            Doc(fromjson("{_id: 4, x: 0}")),
            Doc(fromjson("{_id: 5, x: NumberLong(0)}")),
            Doc(fromjson("{_id: 6, x: 0.123}")),
            Doc(fromjson("{_id: 7, x: 1}")),
            Doc(fromjson("{_id: 8, x: {$maxKey: 1}}"))};
        // Documents in a mock config.cache.chunks collection. Mocked collection boundaries:
        // - [MinKey, hash(0))      : shard1
        // - [hash(0), hash(0) + 1) : shard2
        // - [hash(0) + 1, MaxKey]  : shard3
        std::deque<DocumentSource::GetNextResult> configData{
            Doc{{"_id", Doc{{"x", V(MINKEY)}}},
                {"max", Doc{{"x", getHashedElementValue(0)}}},
                {"shard", "shard1"_sd}},
            Doc{{"_id", Doc{{"x", getHashedElementValue(0)}}},
                {"max", Doc{{"x", getHashedElementValue(0) + 1}}},
                {"shard", "shard2"_sd}},
            Doc{{"_id", Doc{{"x", getHashedElementValue(0) + 1}}},
                {"max", Doc{{"x", V(MAXKEY)}}},
                {"shard", "shard3"_sd}}};
        constexpr auto kExpectedCopiedCount = 4;
        const auto verify = [](auto cursor) {
            auto next = cursor->next();
            ASSERT(next);
            ASSERT_BSONOBJ_BINARY_EQ(BSON("_id" << 3 << "x" << -0.123), next->data.toBson());

            next = cursor->next();
            ASSERT(next);
            ASSERT_BSONOBJ_BINARY_EQ(BSON("_id" << 4 << "x" << 0), next->data.toBson());

            next = cursor->next();
            ASSERT(next);
            ASSERT_BSONOBJ_BINARY_EQ(BSON("_id" << 5 << "x" << 0LL), next->data.toBson());

            next = cursor->next();
            ASSERT(next);
            ASSERT_BSONOBJ_BINARY_EQ(BSON("_id" << 6 << "x" << 0.123), next->data.toBson());

            ASSERT_FALSE(cursor->next());
        };

        runPipelineTest(std::move(sk),
                        ShardId("shard2"),
                        std::move(collectionData),
                        std::move(configData),
                        testOptions,
                        kExpectedCopiedCount,
                        verify);
    }
}

TEST_F(ReshardingCollectionClonerTest, CompoundHashedShardKey) {
    for (const auto& testOptions : makeAllTestOptions()) {
        ShardKeyPattern sk{fromjson("{x: 'hashed', y: 1}")};
        std::deque<DocumentSource::GetNextResult> collectionData{
            Doc(fromjson("{_id: 1, x: {$minKey: 1}}")),
            Doc(fromjson("{_id: 2, x: -1}")),
            Doc(fromjson("{_id: 3, x: -0.123, y: -1}")),
            Doc(fromjson("{_id: 4, x: 0, y: 0}")),
            Doc(fromjson("{_id: 5, x: NumberLong(0), y: 1}")),
            Doc(fromjson("{_id: 6, x: 0.123}")),
            Doc(fromjson("{_id: 7, x: 1}")),
            Doc(fromjson("{_id: 8, x: {$maxKey: 1}}"))};
        // Documents in a mock config.cache.chunks collection. Mocked collection boundaries:
        // - [{x: MinKey, y: MinKey}, {x: hash(0), y: 0}) : shard1
        // - [{x: hash(0), y: 0}, {x: hash(0), y: 1})     : shard2
        // - [{x: hash(0), y: 1}, {x: MaxKey, y: MaxKey}] : shard3
        std::deque<DocumentSource::GetNextResult> configData{
            Doc{{"_id", Doc{{"x", V(MINKEY)}, {"y", V(MINKEY)}}},
                {"max", Doc{{"x", getHashedElementValue(0)}, {"y", 0}}},
                {"shard", "shard1"_sd}},
            Doc{{"_id", Doc{{"x", getHashedElementValue(0)}, {"y", 0}}},
                {"max", Doc{{"x", getHashedElementValue(0)}, {"y", 1}}},
                {"shard", "shard2"_sd}},
            Doc{{"_id", Doc{{"x", getHashedElementValue(0)}, {"y", 1}}},
                {"max", Doc{{"x", V(MAXKEY)}, {"y", V(MAXKEY)}}},
                {"shard", "shard3"_sd}}};
        constexpr auto kExpectedCopiedCount = 1;
        const auto verify = [](auto cursor) {
            auto next = cursor->next();
            ASSERT(next);
            ASSERT_BSONOBJ_BINARY_EQ(BSON("_id" << 4 << "x" << 0 << "y" << 0), next->data.toBson());

            ASSERT_FALSE(cursor->next());
        };

        runPipelineTest(std::move(sk),
                        ShardId("shard2"),
                        std::move(collectionData),
                        std::move(configData),
                        testOptions,
                        kExpectedCopiedCount,
                        verify);
    }
}
}  // namespace
}  // namespace mongo
