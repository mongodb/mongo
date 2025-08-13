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

#include "mongo/db/exec/classic/query_shard_server_test_fixture.h"

#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_runtime.h"

namespace mongo {
namespace {
IndexSpec makeIndexSpec(const BSONObj& index, StringData indexName) {
    IndexSpec spec;
    spec.name(indexName);
    spec.addKeys(index);
    return spec;
}

void validateOutput(const WorkingSet& ws, const WorkingSetID wsid, const BSONObj& expectedKeyData) {
    // For some reason (at least under OS X clang), we cannot refer to INVALID_ID inside the test
    // assertion macro.
    WorkingSetID invalid = WorkingSet::INVALID_ID;
    ASSERT_NOT_EQUALS(invalid, wsid);

    auto member = ws.get(wsid);

    if (member->hasObj()) {
        ASSERT_BSONOBJ_EQ(expectedKeyData, member->doc.value().toBson());
    } else {
        // Key value is retrieved from working set key data instead of RecordId.
        ASSERT_EQ(member->keyData.size(), 1);
        ASSERT_BSONOBJ_EQ(expectedKeyData, member->keyData[0].keyData);
    }
}
}  // namespace

void QueryShardServerTestFixture::doWorkAndValidate(
    PlanStage& stage,
    WorkingSet& ws,
    WorkingSetID& wsid,
    const std::vector<DoWorkResult>& expectedWorkPattern) {
    for (const auto& nextExpectedWork : expectedWorkPattern) {
        const auto nextState = stage.work(&wsid);
        visit(OverloadedVisitor{
                  [&nextState](PlanStage::StageState state) { ASSERT_EQ(nextState, state); },
                  [&nextState, &ws, &wsid](const BSONObj& expectedKeyData) {
                      ASSERT_EQ(nextState, PlanStage::ADVANCED);
                      validateOutput(ws, wsid, expectedKeyData);
                  }},
              nextExpectedWork);
    }
}

void QueryShardServerTestFixture::setUp() {
    ShardServerTestFixture::setUp();
    OperationContext* opCtx = operationContext();

    _testNss = NamespaceString::createNamespaceString_forTest("test_db.distinct_test"_sd);
    _expCtx = std::make_unique<ExpressionContextForTest>(opCtx, _testNss);
    _client = std::make_unique<DBDirectClient>(opCtx);
}

void QueryShardServerTestFixture::createIndex(const BSONObj index, std::string indexName) {
    _client->createCollection(_testNss);
    _client->createIndex(_testNss, makeIndexSpec(index, indexName));
}

void QueryShardServerTestFixture::insertDocs(const std::vector<BSONObj>& docs) {
    // Do batched inserts for larger collection sizes.
    const auto batchSize = docs.size() > 10000 ? 10000 : docs.size();
    write_ops::InsertCommandRequest insertOp(_testNss);
    std::vector<BSONObj> batch;
    batch.reserve(batchSize);
    auto it = docs.begin();
    size_t docsSoFar = 0;
    while (it != docs.end()) {
        size_t numInCurBatch = std::min(batchSize, docs.size() - docsSoFar);
        batch.insert(batch.begin(), it, it + numInCurBatch);
        insertOp.setDocuments(std::move(batch));
        batch.clear();
        auto insertReply = _client->insert(insertOp);
        ASSERT_FALSE(insertReply.getWriteErrors());
        std::advance(it, numInCurBatch);
        docsSoFar += numInCurBatch;
    }
}

const IndexDescriptor& QueryShardServerTestFixture::getIndexDescriptor(const CollectionPtr& coll,
                                                                       StringData indexName) {
    auto* opCtx = operationContext();
    std::vector<const IndexDescriptor*> indexes;
    const auto* idxDesc = coll->getIndexCatalog()->findIndexByName(opCtx, indexName);
    ASSERT_NE(idxDesc, nullptr);
    return *idxDesc;
}

CollectionMetadata QueryShardServerTestFixture::prepareTestData(
    const KeyPattern& shardKeyPattern, const std::vector<ChunkDesc>& chunkDescs) {
    const UUID uuid = UUID::gen();
    const OID epoch = OID::gen();

    ShardId curShard("0");
    ShardId otherShard("1");
    ChunkVersion version({epoch, Timestamp(1, 1)}, {1, 0});

    _chunks.clear();
    _chunks.reserve(chunkDescs.size());
    for (auto&& chunkDesc : chunkDescs) {
        ChunkType c{uuid, chunkDesc.range, version, chunkDesc.isOnCurShard ? curShard : otherShard};
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

    ChunkManager cm(makeStandaloneRoutingTableHistory(std::move(rt)), boost::none);
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
}  // namespace mongo
