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

#include <memory>

#include "mongo/client/index_spec.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/distinct_scan.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/metadata_manager.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/shard_version_factory.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/overloaded_visitor.h"

namespace mongo {
namespace {
class DistinctScanTest : public ShardServerTestFixture {
public:
    static IndexBounds makeIndexBounds(
        std::vector<std::pair<std::string, std::vector<Interval>>>&& protoOils) {
        IndexBounds bounds;
        bounds.isSimpleRange = true;
        for (auto&& [fieldName, intervals] : protoOils) {
            OrderedIntervalList oil(fieldName);
            oil.intervals = std::move(intervals);
            bounds.fields.push_back(oil);
        }
        return bounds;
    }

    static IndexSpec makeIndexSpec(const BSONObj& index, StringData indexName) {
        IndexSpec spec;
        spec.name(indexName);
        spec.addKeys(index);
        return spec;
    }

    static void validateOutput(const WorkingSet& ws,
                               const WorkingSetID wsid,
                               const BSONObj& expectedKeyData,
                               bool shouldFetch) {
        // For some reason (at least under OS X clang), we cannot refer to INVALID_ID
        // inside the test assertion macro.
        WorkingSetID invalid = WorkingSet::INVALID_ID;
        ASSERT_NOT_EQUALS(invalid, wsid);

        auto member = ws.get(wsid);

        if (shouldFetch) {
            ASSERT_TRUE(member->hasObj());
            ASSERT_BSONOBJ_EQ(expectedKeyData, member->doc.value().toBson());
        } else {
            // Key value is retrieved from working set key data
            // instead of RecordId.
            ASSERT_FALSE(member->hasObj());
            ASSERT_EQ(member->keyData.size(), 1);
            ASSERT_BSONOBJ_EQ(expectedKeyData, member->keyData[0].keyData);
        }
    }

    using DoWorkResult = std::variant<PlanStage::StageState, BSONObj>;
    static void doWorkAndValidate(DistinctScan& scan,
                                  WorkingSet& ws,
                                  WorkingSetID& wsid,
                                  const std::vector<DoWorkResult>& expectedWorkPattern,
                                  bool shouldFetch) {
        for (const auto& nextExpectedWork : expectedWorkPattern) {
            const auto nextState = scan.work(&wsid);
            visit(OverloadedVisitor{
                      [&nextState](PlanStage::StageState state) { ASSERT_EQ(nextState, state); },
                      [shouldFetch, &nextState, &ws, &wsid](const BSONObj& expectedKeyData) {
                          ASSERT_EQ(nextState, PlanStage::ADVANCED);
                          validateOutput(ws, wsid, expectedKeyData, shouldFetch);
                      }},
                  nextExpectedWork);
        }
    }

    void setUp() final {
        ShardServerTestFixture::setUp();
        OperationContext* opCtx = operationContext();

        _testNss = NamespaceString::createNamespaceString_forTest("test_db.distinct_test"_sd);
        _expCtx = std::make_unique<ExpressionContextForTest>(opCtx, _testNss);
        _client = std::make_unique<DBDirectClient>(opCtx);
    }

    auto* expressionContext() {
        return _expCtx.get();
    }

    const auto& nss() {
        return _testNss;
    }

    /**
     * Create a simple 'index' on an empty collection with name 'indexName'.
     */
    void createIndex(const BSONObj index, std::string indexName) {
        _client->createIndex(_testNss, makeIndexSpec(index, indexName));
    }

    /**
     * Insert the given documents into the test collection.
     */
    void insertDocs(const std::vector<BSONObj>& docs) {
        const size_t expectedSize = docs.size();
        write_ops::InsertCommandRequest insertOp(_testNss);
        insertOp.setDocuments(std::move(docs));
        auto insertReply = _client->insert(insertOp);
        ASSERT_FALSE(insertReply.getWriteErrors());
        ASSERT_EQ(insertReply.getN(), expectedSize);
    }

    /**
     * Get the index descriptor for the provided 'index'. Asserts if index isn't found.
     */
    const IndexDescriptor& getIndexDescriptor(const CollectionPtr& coll, StringData indexName) {
        auto* opCtx = operationContext();
        std::vector<const IndexDescriptor*> indexes;
        const auto* idxDesc = coll->getIndexCatalog()->findIndexByName(opCtx, indexName);
        ASSERT_NE(idxDesc, nullptr);
        return *idxDesc;
    }

    struct ChunkDesc {
        ChunkRange range;
        bool isOnCurShard;
    };

    CollectionMetadata prepareTestData(const KeyPattern& shardKeyPattern,
                                       const std::vector<ChunkDesc>& chunkDescs) {
        const UUID uuid = UUID::gen();
        const OID epoch = OID::gen();

        ShardId curShard("0");
        ShardId otherShard("1");
        ChunkVersion version({epoch, Timestamp(1, 1)}, {1, 0});

        _chunks.clear();
        _chunks.reserve(chunkDescs.size());
        for (auto&& chunkDesc : chunkDescs) {
            ChunkType c{
                uuid, chunkDesc.range, version, chunkDesc.isOnCurShard ? curShard : otherShard};
            _chunks.push_back(c);
        };

        auto rt = RoutingTableHistory::makeNew(_testNss,
                                               uuid,
                                               shardKeyPattern,
                                               false, /* unsplittable */
                                               nullptr,
                                               false,
                                               epoch,
                                               Timestamp(1, 1),
                                               boost::none /* timeseriesFields */,
                                               boost::none /* reshardingFields */,
                                               true,
                                               _chunks);

        ChunkManager cm(curShard,
                        DatabaseVersion(UUID::gen(), Timestamp(1, 1)),
                        makeStandaloneRoutingTableHistory(std::move(rt)),
                        boost::none);
        ASSERT_EQ(_chunks.size(), cm.numChunks());

        {
            AutoGetCollection autoColl(operationContext(), _testNss, MODE_X);
            auto scopedCsr = CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(
                operationContext(), _testNss);
            scopedCsr->setFilteringMetadata(operationContext(), CollectionMetadata(cm, curShard));
        }

        _manager = std::make_shared<MetadataManager>(
            getServiceContext(), _testNss, CollectionMetadata(cm, curShard));

        return CollectionMetadata(std::move(cm), curShard);
    }

    struct DistinctScanParamsForTest {
        // Collection & sharding set-up.
        const KeyPattern& shardKey;
        const std::vector<BSONObj>& docsOnShard;
        const std::vector<ChunkDesc>& chunks;

        // Distinct scan params.
        BSONObj idxKey;
        int scanDirection;
        int fieldNo;
        IndexBounds bounds;
        bool shouldShardFilter;
        bool shouldFetch;

        // Expected output of distinct scan (from repeated calls to work()).
        const std::vector<DoWorkResult>& expectedWorkPattern;
    };

    void verifyDistinctScanExecution(DistinctScanParamsForTest&& testParams) {
        createIndex(testParams.idxKey, "some_index");
        insertDocs(testParams.docsOnShard);

        const auto metadata{prepareTestData(testParams.shardKey, testParams.chunks)};

        auto opCtx = operationContext();
        auto ns = nss();

        AutoGetCollectionForReadCommand ctx(opCtx, ns);
        const CollectionPtr& coll = ctx.getCollection();
        const auto& idxDesc = getIndexDescriptor(coll, "some_index");

        // Set-up DistinctParams for a full distinct scan on the first field in the index.
        DistinctParams params{opCtx, coll, &idxDesc};
        params.scanDirection = testParams.scanDirection;
        params.fieldNo = testParams.fieldNo;
        params.bounds = std::move(testParams.bounds);

        // Create a shard filterer.
        ScopedSetShardRole scopedSetShardRole{
            opCtx,
            ns,
            ShardVersionFactory::make(
                metadata, boost::optional<CollectionIndexes>(boost::none)) /* shardVersion */,
            boost::none /* databaseVersion */};
        auto scopedCss = CollectionShardingState::assertCollectionLockedAndAcquire(opCtx, ns);
        auto sfi = testParams.shouldShardFilter
            ? std::make_unique<ShardFiltererImpl>(scopedCss->getOwnershipFilter(
                  opCtx, CollectionShardingState::OrphanCleanupPolicy::kDisallowOrphanCleanup))
            : nullptr;

        // Construct distinct, and verify its expected execution pattern on the given data.
        WorkingSet ws;
        WorkingSetID wsid;
        DistinctScan distinct(expressionContext(),
                              &coll,
                              std::move(params),
                              &ws,
                              std::move(sfi),
                              testParams.shouldFetch);
        doWorkAndValidate(
            distinct, ws, wsid, testParams.expectedWorkPattern, testParams.shouldFetch);
    }

private:
    std::vector<ChunkType> _chunks;
    std::shared_ptr<MetadataManager> _manager;
    std::unique_ptr<ExpressionContextForTest> _expCtx;
    std::unique_ptr<DBDirectClient> _client;
    NamespaceString _testNss;
};

const std::vector<BSONObj> kDataset = {
    BSON("_id" << 1 << "a" << 5 << "b" << true << "c"
               << "apple"),
    BSON("_id" << 2 << "a" << 5 << "b" << false << "c"
               << "apple"),
    BSON("_id" << 3 << "a" << 5 << "b" << true << "c"
               << "AppLe"),
    BSON("_id" << 4 << "a" << 9 << "b" << false << "c"
               << "ApPlE"),
    BSON("_id" << 5 << "a" << 10 << "b" << true << "c"
               << "PEAR"),
    BSON("_id" << 6 << "a" << 10 << "b" << false << "c"
               << "pear"),
    BSON("_id" << 7 << "a" << 15 << "b" << true << "c"
               << "pEaR"),
    BSON("_id" << 8 << "a" << 19 << "b" << false << "c"
               << "PeAr"),
    BSON("_id" << 9 << "a" << 20 << "b" << true << "c"
               << "baNaNa"),
    BSON("_id" << 10 << "a" << 20 << "b" << false << "c"
               << "BaNaNa"),
    BSON("_id" << 11 << "a" << 20 << "b" << true << "c"
               << "BANANA"),
    BSON("_id" << 12 << "a" << 20 << "b" << false << "c"
               << "ananas"),
    BSON("_id" << 13 << "a" << 25 << "b" << true << "c"
               << "ananas"),
    BSON("_id" << 14 << "a" << 35 << "b" << false << "c"
               << "ANANAS"),
};

TEST_F(DistinctScanTest, NoShardFilteringTest) {
    const ShardKeyPattern shardKeyPattern(BSON("b" << 1));
    const KeyPattern& shardKey = shardKeyPattern.getKeyPattern();
    verifyDistinctScanExecution(
        {.shardKey = shardKey,
         .docsOnShard = kDataset,
         .chunks = {{{shardKey.globalMin(), shardKey.globalMax()}, true /* isOnCurShard */}},
         .idxKey = shardKey.toBSON(),
         .scanDirection = 1,
         .fieldNo = 0,
         .bounds = makeIndexBounds({{"b", {IndexBoundsBuilder::allValues()}}}),
         .shouldShardFilter = false,
         .shouldFetch = false,
         .expectedWorkPattern = {BSON("" << false), BSON("" << true), PlanStage::IS_EOF}});
}

TEST_F(DistinctScanTest, NoShardFilteringReverseScanTest) {
    const ShardKeyPattern shardKeyPattern(BSON("b" << 1));
    const KeyPattern& shardKey = shardKeyPattern.getKeyPattern();
    verifyDistinctScanExecution(
        {.shardKey = shardKey,
         .docsOnShard = kDataset,
         .chunks = {{{shardKey.globalMin(), shardKey.globalMax()}, true /* isOnCurShard */}},
         .idxKey = shardKey.toBSON(),
         .scanDirection = -1,
         .fieldNo = 0,
         .bounds = makeIndexBounds({{"b", {IndexBoundsBuilder::allValues().reverseClone()}}}),
         .shouldShardFilter = false,
         .shouldFetch = false,
         .expectedWorkPattern = {BSON("" << true), BSON("" << false), PlanStage::IS_EOF}});
}

TEST_F(DistinctScanTest, NoShardFilteringFetchTest) {
    const ShardKeyPattern shardKeyPattern(BSON("b" << 1));
    const KeyPattern& shardKey = shardKeyPattern.getKeyPattern();
    verifyDistinctScanExecution(
        {.shardKey = shardKey,
         .docsOnShard = kDataset,
         .chunks = {{{shardKey.globalMin(), shardKey.globalMax()}, true /* isOnCurShard */}},
         .idxKey = shardKey.toBSON(),
         .scanDirection = 1,
         .fieldNo = 0,
         .bounds = makeIndexBounds({{"b", {IndexBoundsBuilder::allValues()}}}),
         .shouldShardFilter = false,
         .shouldFetch = true,
         .expectedWorkPattern = {BSON("_id" << 2 << "a" << 5 << "b" << false << "c"
                                            << "apple"),
                                 BSON("_id" << 1 << "a" << 5 << "b" << true << "c"
                                            << "apple"),
                                 PlanStage::IS_EOF}});
}

TEST_F(DistinctScanTest, NoShardFilteringReverseScanFetchTest) {
    const ShardKeyPattern shardKeyPattern(BSON("b" << 1));
    const KeyPattern& shardKey = shardKeyPattern.getKeyPattern();
    verifyDistinctScanExecution(
        {.shardKey = shardKey,
         .docsOnShard = kDataset,
         .chunks = {{{shardKey.globalMin(), shardKey.globalMax()}, true /* isOnCurShard */}},
         .idxKey = shardKey.toBSON(),
         .scanDirection = -1,
         .fieldNo = 0,
         .bounds = makeIndexBounds({{"b", {IndexBoundsBuilder::allValues().reverseClone()}}}),
         .shouldShardFilter = false,
         .shouldFetch = true,
         .expectedWorkPattern = {BSON("_id" << 13 << "a" << 25 << "b" << true << "c"
                                            << "ananas"),
                                 BSON("_id" << 14 << "a" << 35 << "b" << false << "c"
                                            << "ANANAS"),
                                 PlanStage::IS_EOF}});
}

TEST_F(DistinctScanTest, ShardFilteringNoOrphansTest) {
    const ShardKeyPattern shardKeyPattern(BSON("b" << 1));
    const KeyPattern& shardKey = shardKeyPattern.getKeyPattern();
    verifyDistinctScanExecution(
        {.shardKey = shardKey,
         .docsOnShard = kDataset,
         .chunks = {{{shardKey.globalMin(), shardKey.globalMax()}, true /* isOnCurShard */}},
         .idxKey = shardKey.toBSON(),
         .scanDirection = -1,
         .fieldNo = 0,
         .bounds = makeIndexBounds({{"b", {IndexBoundsBuilder::allValues().reverseClone()}}}),
         .shouldShardFilter = true,
         .shouldFetch = false,
         .expectedWorkPattern = {BSON("" << true), BSON("" << false), PlanStage::IS_EOF}});
}

TEST_F(DistinctScanTest, ShardFilteringShardKeyIsIndexKeyTest) {
    const ShardKeyPattern shardKeyPattern(BSON("a" << 1));
    const KeyPattern& shardKey = shardKeyPattern.getKeyPattern();
    verifyDistinctScanExecution(
        {.shardKey = shardKey,
         .docsOnShard = kDataset,
         .chunks =
             {
                 {{shardKey.globalMin(), BSON("a" << 0)}, false /* isOnCurShard */},
                 {{BSON("a" << 0), BSON("a" << 10)}, true /* isOnCurShard */},
                 {{BSON("a" << 10), BSON("a" << 20)}, false /* isOnCurShard */},
                 {{BSON("a" << 20), BSON("a" << 30)}, true /* isOnCurShard */},
                 {{BSON("a" << 30), shardKey.globalMax()}, false /* isOnCurShard */},
             },
         .idxKey = shardKey.toBSON(),
         .scanDirection = 1,
         .fieldNo = 0,
         .bounds = makeIndexBounds({{"a", {IndexBoundsBuilder::allValues()}}}),
         .shouldShardFilter = true,
         .shouldFetch = false,
         .expectedWorkPattern = {// See first unique value, not an orphan, so we seek.
                                 BSON("" << 5),
                                 // Same here.
                                 BSON("" << 9),
                                 PlanStage::NEED_TIME,
                                 PlanStage::NEED_TIME,
                                 PlanStage::NEED_TIME,
                                 PlanStage::NEED_TIME,
                                 // Finally find non-orphan; back to seeking.
                                 BSON("" << 20),
                                 BSON("" << 25),
                                 // Last doc is an orphan- call next().
                                 PlanStage::NEED_TIME,
                                 PlanStage::IS_EOF}});
}

TEST_F(DistinctScanTest, ShardFilteringShardKeyIsReverseOfIndexKeyReverseScanTest) {
    const ShardKeyPattern shardKeyPattern(BSON("a" << 1));
    const KeyPattern& shardKey = shardKeyPattern.getKeyPattern();
    verifyDistinctScanExecution(
        {.shardKey = shardKey,
         .docsOnShard = kDataset,
         .chunks =
             {
                 {{shardKey.globalMin(), BSON("a" << 0)}, false /* isOnCurShard */},
                 {{BSON("a" << 0), BSON("a" << 10)}, true /* isOnCurShard */},
                 {{BSON("a" << 10), BSON("a" << 20)}, false /* isOnCurShard */},
                 {{BSON("a" << 20), BSON("a" << 30)}, true /* isOnCurShard */},
                 {{BSON("a" << 30), shardKey.globalMax()}, false /* isOnCurShard */},
             },
         .idxKey = BSON("a" << -1),
         .scanDirection = -1,
         .fieldNo = 0,
         .bounds = makeIndexBounds({{"a", {IndexBoundsBuilder::allValues()}}}),
         .shouldShardFilter = true,
         .shouldFetch = false,
         // Same pattern as above.
         .expectedWorkPattern = {BSON("" << 5),
                                 BSON("" << 9),
                                 PlanStage::NEED_TIME,
                                 PlanStage::NEED_TIME,
                                 PlanStage::NEED_TIME,
                                 PlanStage::NEED_TIME,
                                 BSON("" << 20),
                                 BSON("" << 25),
                                 PlanStage::NEED_TIME,
                                 PlanStage::IS_EOF}});
}

TEST_F(DistinctScanTest, ShardFilteringShardKeyIsPrefixDistinctKeyPrefixTest) {
    const ShardKeyPattern shardKeyPattern(BSON("a" << 1));
    const KeyPattern& shardKey = shardKeyPattern.getKeyPattern();
    verifyDistinctScanExecution(
        {.shardKey = shardKey,
         .docsOnShard = kDataset,
         .chunks =
             {
                 {{shardKey.globalMin(), BSON("a" << 0)}, false /* isOnCurShard */},
                 {{BSON("a" << 0), BSON("a" << 10)}, true /* isOnCurShard */},
                 {{BSON("a" << 10), BSON("a" << 20)}, false /* isOnCurShard */},
                 {{BSON("a" << 20), BSON("a" << 30)}, true /* isOnCurShard */},
                 {{BSON("a" << 30), shardKey.globalMax()}, false /* isOnCurShard */},
             },
         .idxKey = BSON("a" << 1 << "b" << 1 << "c" << 1),
         .scanDirection = 1,
         .fieldNo = 0,
         .bounds = makeIndexBounds({{"a", {IndexBoundsBuilder::allValues()}},
                                    {"b", {IndexBoundsBuilder::allValues()}},
                                    {"c", {IndexBoundsBuilder::allValues()}}}),
         .shouldShardFilter = true,
         .shouldFetch = false,
         // Same pattern as above (but with more fields).
         .expectedWorkPattern = {BSON("" << 5 << "" << false << ""
                                         << "apple"),
                                 BSON("" << 9 << "" << false << ""
                                         << "ApPlE"),
                                 PlanStage::NEED_TIME,
                                 PlanStage::NEED_TIME,
                                 PlanStage::NEED_TIME,
                                 PlanStage::NEED_TIME,
                                 BSON("" << 20 << "" << false << ""
                                         << "BaNaNa"),
                                 BSON("" << 25 << "" << true << ""
                                         << "ananas"),
                                 PlanStage::NEED_TIME,
                                 PlanStage::IS_EOF}});
}

TEST_F(DistinctScanTest, ShardFilteringShardKeyIsPrefixDistinctKeyMiddleTest) {
    const ShardKeyPattern shardKeyPattern(BSON("a" << 1));
    const KeyPattern& shardKey = shardKeyPattern.getKeyPattern();
    verifyDistinctScanExecution(
        {.shardKey = shardKey,
         .docsOnShard = kDataset,
         .chunks =
             {
                 {{shardKey.globalMin(), BSON("a" << 0)}, false /* isOnCurShard */},
                 {{BSON("a" << 0), BSON("a" << 10)}, true /* isOnCurShard */},
                 {{BSON("a" << 10), BSON("a" << 20)}, false /* isOnCurShard */},
                 {{BSON("a" << 20), BSON("a" << 30)}, true /* isOnCurShard */},
                 {{BSON("a" << 30), shardKey.globalMax()}, false /* isOnCurShard */},
             },
         .idxKey = BSON("a" << 1 << "b" << 1 << "c" << 1),
         .scanDirection = 1,
         .fieldNo = 1,
         .bounds = makeIndexBounds({{"a", {IndexBoundsBuilder::allValues()}},
                                    {"b", {IndexBoundsBuilder::allValues()}},
                                    {"c", {IndexBoundsBuilder::allValues()}}}),
         .shouldShardFilter = true,
         .shouldFetch = false,
         // Distinct is now on second field.
         .expectedWorkPattern = {BSON("" << 5 << "" << false << ""
                                         << "apple"),
                                 BSON("" << 5 << "" << true << ""
                                         << "AppLe"),
                                 BSON("" << 9 << "" << false << ""
                                         << "ApPlE"),
                                 PlanStage::NEED_TIME,
                                 PlanStage::NEED_TIME,
                                 PlanStage::NEED_TIME,
                                 PlanStage::NEED_TIME,
                                 BSON("" << 20 << "" << false << ""
                                         << "BaNaNa"),
                                 BSON("" << 20 << "" << true << ""
                                         << "BANANA"),
                                 BSON("" << 25 << "" << true << ""
                                         << "ananas"),
                                 PlanStage::NEED_TIME,
                                 PlanStage::IS_EOF}});
}

TEST_F(DistinctScanTest, ShardFilteringShardKeyIsPrefixDistinctKeyEndTest) {
    const ShardKeyPattern shardKeyPattern(BSON("a" << 1));
    const KeyPattern& shardKey = shardKeyPattern.getKeyPattern();
    verifyDistinctScanExecution(
        {.shardKey = shardKey,
         .docsOnShard = kDataset,
         .chunks =
             {
                 {{shardKey.globalMin(), BSON("a" << 0)}, false /* isOnCurShard */},
                 {{BSON("a" << 0), BSON("a" << 10)}, true /* isOnCurShard */},
                 {{BSON("a" << 10), BSON("a" << 20)}, false /* isOnCurShard */},
                 {{BSON("a" << 20), BSON("a" << 30)}, true /* isOnCurShard */},
                 {{BSON("a" << 30), shardKey.globalMax()}, false /* isOnCurShard */},
             },
         .idxKey = BSON("a" << 1 << "b" << 1 << "c" << 1),
         .scanDirection = 1,
         .fieldNo = 2,
         .bounds = makeIndexBounds({{"a", {IndexBoundsBuilder::allValues()}},
                                    {"b", {IndexBoundsBuilder::allValues()}},
                                    {"c", {IndexBoundsBuilder::allValues()}}}),
         .shouldShardFilter = true,
         .shouldFetch = false,
         // Distinct is now on third field.
         .expectedWorkPattern = {BSON("" << 5 << "" << false << ""
                                         << "apple"),
                                 BSON("" << 5 << "" << true << ""
                                         << "AppLe"),
                                 BSON("" << 5 << "" << true << ""
                                         << "apple"),
                                 BSON("" << 9 << "" << false << ""
                                         << "ApPlE"),
                                 PlanStage::NEED_TIME,
                                 PlanStage::NEED_TIME,
                                 PlanStage::NEED_TIME,
                                 PlanStage::NEED_TIME,
                                 BSON("" << 20 << "" << false << ""
                                         << "BaNaNa"),
                                 BSON("" << 20 << "" << false << ""
                                         << "ananas"),
                                 BSON("" << 20 << "" << true << ""
                                         << "BANANA"),
                                 BSON("" << 20 << "" << true << ""
                                         << "baNaNa"),
                                 BSON("" << 25 << "" << true << ""
                                         << "ananas"),
                                 PlanStage::NEED_TIME,
                                 PlanStage::IS_EOF}});
}

TEST_F(DistinctScanTest, ShardFilteringShardKeyInMiddleDistinctKeyPrefixTest) {
    const ShardKeyPattern shardKeyPattern(BSON("b" << 1));
    const KeyPattern& shardKey = shardKeyPattern.getKeyPattern();
    verifyDistinctScanExecution(
        {.shardKey = shardKey,
         .docsOnShard = kDataset,
         .chunks =
             {
                 {{shardKey.globalMin(), BSON("b" << true)}, false /* isOnCurShard */},
                 {{BSON("b" << true), shardKey.globalMax()}, true /* isOnCurShard */},
             },
         .idxKey = BSON("a" << 1 << "b" << 1 << "c" << 1),
         .scanDirection = 1,
         .fieldNo = 0,
         .bounds = makeIndexBounds({{"a", {IndexBoundsBuilder::allValues()}},
                                    {"b", {IndexBoundsBuilder::allValues()}},
                                    {"c", {IndexBoundsBuilder::allValues()}}}),
         .shouldShardFilter = true,
         .shouldFetch = false,
         // Only 'true" values f "b" are owned by this shard; note that 'false' always sorts first.
         .expectedWorkPattern = {PlanStage::NEED_TIME,
                                 BSON("" << 5 << "" << true << ""
                                         << "AppLe"),
                                 PlanStage::NEED_TIME,
                                 PlanStage::NEED_TIME,
                                 BSON("" << 10 << "" << true << ""
                                         << "PEAR"),
                                 BSON("" << 15 << "" << true << ""
                                         << "pEaR"),
                                 PlanStage::NEED_TIME,
                                 PlanStage::NEED_TIME,
                                 PlanStage::NEED_TIME,
                                 BSON("" << 20 << "" << true << ""
                                         << "BANANA"),
                                 BSON("" << 25 << "" << true << ""
                                         << "ananas"),
                                 PlanStage::NEED_TIME,
                                 PlanStage::IS_EOF}});
}

TEST_F(DistinctScanTest, ShardFilteringShardKeyInMiddleDistinctKeySuffixTest) {
    const ShardKeyPattern shardKeyPattern(BSON("b" << 1));
    const KeyPattern& shardKey = shardKeyPattern.getKeyPattern();
    verifyDistinctScanExecution(
        {.shardKey = shardKey,
         .docsOnShard = kDataset,
         .chunks =
             {
                 {{shardKey.globalMin(), BSON("b" << true)}, false /* isOnCurShard */},
                 {{BSON("b" << true), shardKey.globalMax()}, true /* isOnCurShard */},
             },
         .idxKey = BSON("a" << 1 << "b" << 1 << "c" << 1),
         .scanDirection = 1,
         .fieldNo = 2,
         .bounds = makeIndexBounds({{"a", {IndexBoundsBuilder::allValues()}},
                                    {"b", {IndexBoundsBuilder::allValues()}},
                                    {"c", {IndexBoundsBuilder::allValues()}}}),
         .shouldShardFilter = true,
         .shouldFetch = false,
         // Similar to above, only our distinct key is now the final field.
         .expectedWorkPattern = {PlanStage::NEED_TIME,
                                 BSON("" << 5 << "" << true << ""
                                         << "AppLe"),
                                 BSON("" << 5 << "" << true << ""
                                         << "apple"),
                                 PlanStage::NEED_TIME,
                                 PlanStage::NEED_TIME,
                                 BSON("" << 10 << "" << true << ""
                                         << "PEAR"),
                                 BSON("" << 15 << "" << true << ""
                                         << "pEaR"),
                                 PlanStage::NEED_TIME,
                                 PlanStage::NEED_TIME,
                                 PlanStage::NEED_TIME,
                                 BSON("" << 20 << "" << true << ""
                                         << "BANANA"),
                                 BSON("" << 20 << "" << true << ""
                                         << "baNaNa"),
                                 BSON("" << 25 << "" << true << ""
                                         << "ananas"),
                                 PlanStage::NEED_TIME,
                                 PlanStage::IS_EOF}});
}


TEST_F(DistinctScanTest, ShardFilteringShardKeyAtEndTest) {
    const ShardKeyPattern shardKeyPattern(BSON("c" << 1));
    const KeyPattern& shardKey = shardKeyPattern.getKeyPattern();
    verifyDistinctScanExecution({.shardKey = shardKey,
                                 .docsOnShard = kDataset,
                                 .chunks =
                                     {
                                         {{shardKey.globalMin(),
                                           BSON("c"
                                                << "PEAR")},
                                          true /* isOnCurShard */},
                                         {{BSON("c"
                                                << "PEAR"),
                                           BSON("c"
                                                << "banana")},
                                          false /* isOnCurShard */},
                                         {{BSON("c"
                                                << "banana"),
                                           shardKey.globalMax()},
                                          true /* isOnCurShard */},
                                     },
                                 .idxKey = BSON("a" << -1 << "b" << -1 << "c" << -1),
                                 .scanDirection = -1,
                                 .fieldNo = 0,
                                 .bounds =
                                     makeIndexBounds({{"a", {IndexBoundsBuilder::allValues()}},
                                                      {"b", {IndexBoundsBuilder::allValues()}},
                                                      {"c", {IndexBoundsBuilder::allValues()}}}),
                                 .shouldShardFilter = true,
                                 .shouldFetch = false,
                                 .expectedWorkPattern = {PlanStage::NEED_TIME,
                                                         BSON("" << 5 << "" << true << ""
                                                                 << "AppLe"),
                                                         BSON("" << 9 << "" << false << ""
                                                                 << "ApPlE"),
                                                         BSON("" << 10 << "" << false << ""
                                                                 << "pear"),
                                                         BSON("" << 15 << "" << true << ""
                                                                 << "pEaR"),
                                                         PlanStage::NEED_TIME,
                                                         BSON("" << 20 << "" << false << ""
                                                                 << "BaNaNa"),
                                                         PlanStage::NEED_TIME,
                                                         BSON("" << 35 << "" << false << ""
                                                                 << "ANANAS"),
                                                         PlanStage::IS_EOF}});
}

TEST_F(DistinctScanTest, ShardFilteringCompoundShardKeySubsetTest) {
    const ShardKeyPattern shardKeyPattern(BSON("a" << 1 << "c" << 1));
    const KeyPattern& shardKey = shardKeyPattern.getKeyPattern();
    verifyDistinctScanExecution(
        {.shardKey = shardKey,
         .docsOnShard = kDataset,
         .chunks =
             {
                 {{shardKey.globalMin(),
                   BSON("a" << 10 << "c"
                            << "banana")},
                  false /* isOnCurShard */},
                 {{BSON("a" << 10 << "c"
                            << "banana"),
                   BSON("a" << 20 << "c"
                            << "banana")},
                  true /* isOnCurShard */},
                 {{BSON("a" << 20 << "c"
                            << "banana"),
                   shardKey.globalMax()},
                  false /* isOnCurShard */},
             },
         .idxKey = BSON("a" << 1 << "b" << -1 << "c" << -1),
         .scanDirection = 1,
         .fieldNo = 0,
         .bounds = makeIndexBounds(
             {{"a",
               {IndexBoundsBuilder::makeRangeInterval(
                   BSON("" << 0 << "" << 19), BoundInclusion::kIncludeBothStartAndEndKeys)}},
              {"b", {IndexBoundsBuilder::allValues().reverseClone()}},
              {"c", {IndexBoundsBuilder::allValues().reverseClone()}}}),
         .shouldShardFilter = true,
         .shouldFetch = false,
         .expectedWorkPattern = {PlanStage::NEED_TIME,
                                 PlanStage::NEED_TIME,
                                 PlanStage::NEED_TIME,
                                 PlanStage::NEED_TIME,
                                 PlanStage::NEED_TIME,
                                 BSON("" << 10 << "" << false << ""
                                         << "pear"),
                                 BSON("" << 15 << "" << true << ""
                                         << "pEaR"),
                                 BSON("" << 19 << "" << false << ""
                                         << "PeAr"),
                                 PlanStage::IS_EOF}});
}

TEST_F(DistinctScanTest, ShardFilteringNoShardKeyInIndexKeyTest) {
    const ShardKeyPattern shardKeyPattern(BSON("c" << 1));
    const KeyPattern& shardKey = shardKeyPattern.getKeyPattern();
    verifyDistinctScanExecution({.shardKey = shardKey,
                                 .docsOnShard = kDataset,
                                 .chunks =
                                     {
                                         {{shardKey.globalMin(),
                                           BSON("c"
                                                << "PEAR")},
                                          true /* isOnCurShard */},
                                         {{BSON("c"
                                                << "PEAR"),
                                           BSON("c"
                                                << "banana")},
                                          false /* isOnCurShard */},
                                         {{BSON("c"
                                                << "banana"),
                                           shardKey.globalMax()},
                                          true /* isOnCurShard */},
                                     },
                                 .idxKey = BSON("a" << 1),
                                 .scanDirection = 1,
                                 .fieldNo = 0,
                                 .bounds =
                                     makeIndexBounds({{"a", {IndexBoundsBuilder::allValues()}}}),
                                 .shouldShardFilter = true,
                                 .shouldFetch = true,
                                 .expectedWorkPattern = {
                                     PlanStage::NEED_TIME,
                                     PlanStage::NEED_TIME,
                                     BSON("_id" << 3 << "a" << 5 << "b" << true << "c"
                                                << "AppLe"),
                                     BSON("_id" << 4 << "a" << 9 << "b" << false << "c"
                                                << "ApPlE"),
                                     PlanStage::NEED_TIME,
                                     BSON("_id" << 6 << "a" << 10 << "b" << false << "c"
                                                << "pear"),
                                     BSON("_id" << 7 << "a" << 15 << "b" << true << "c"
                                                << "pEaR"),
                                     PlanStage::NEED_TIME,
                                     PlanStage::NEED_TIME,
                                     BSON("_id" << 10 << "a" << 20 << "b" << false << "c"
                                                << "BaNaNa"),
                                     PlanStage::NEED_TIME,
                                     BSON("_id" << 14 << "a" << 35 << "b" << false << "c"
                                                << "ANANAS"),
                                     PlanStage::IS_EOF}});
}

TEST_F(DistinctScanTest, ShardFilteringNoShardKeyInIndexKeyReverseScanTest) {
    const ShardKeyPattern shardKeyPattern(BSON("c" << 1));
    const KeyPattern& shardKey = shardKeyPattern.getKeyPattern();
    verifyDistinctScanExecution({.shardKey = shardKey,
                                 .docsOnShard = kDataset,
                                 .chunks =
                                     {
                                         {{shardKey.globalMin(),
                                           BSON("c"
                                                << "PEAR")},
                                          true /* isOnCurShard */},
                                         {{BSON("c"
                                                << "PEAR"),
                                           BSON("c"
                                                << "banana")},
                                          false /* isOnCurShard */},
                                         {{BSON("c"
                                                << "banana"),
                                           shardKey.globalMax()},
                                          true /* isOnCurShard */},
                                     },
                                 .idxKey = BSON("a" << 1),
                                 .scanDirection = -1,
                                 .fieldNo = 0,
                                 .bounds = makeIndexBounds(
                                     {{"a", {IndexBoundsBuilder::allValues().reverseClone()}}}),
                                 .shouldShardFilter = true,
                                 .shouldFetch = true,
                                 .expectedWorkPattern = {
                                     BSON("_id" << 14 << "a" << 35 << "b" << false << "c"
                                                << "ANANAS"),
                                     PlanStage::NEED_TIME,
                                     PlanStage::NEED_TIME,
                                     BSON("_id" << 11 << "a" << 20 << "b" << true << "c"
                                                << "BANANA"),
                                     PlanStage::NEED_TIME,
                                     BSON("_id" << 7 << "a" << 15 << "b" << true << "c"
                                                << "pEaR"),
                                     BSON("_id" << 6 << "a" << 10 << "b" << false << "c"
                                                << "pear"),
                                     BSON("_id" << 4 << "a" << 9 << "b" << false << "c"
                                                << "ApPlE"),
                                     BSON("_id" << 3 << "a" << 5 << "b" << true << "c"
                                                << "AppLe"),
                                     PlanStage::IS_EOF}});
}

}  // namespace
}  // namespace mongo
