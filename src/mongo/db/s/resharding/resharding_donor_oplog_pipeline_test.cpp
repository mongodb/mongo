/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/s/resharding/resharding_donor_oplog_pipeline.h"

#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/s/resharding/donor_oplog_id_gen.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <fmt/format.h>

namespace mongo {
namespace {

const NamespaceString kLocalOplogBufferNss = NamespaceString::createNamespaceString_forTest(
    fmt::format("config", "{}xxx.yyy", NamespaceString::kReshardingLocalOplogBufferPrefix));

/**
 * Mock interface to allow specifiying mock results for the lookup pipeline.
 */
class MockMongoInterface final : public StubMongoProcessInterface {
public:
    MockMongoInterface(std::deque<DocumentSource::GetNextResult> mockResults)
        : _mockResults(std::move(mockResults)) {}

    std::unique_ptr<Pipeline> attachCursorSourceToPipelineForLocalRead(
        std::unique_ptr<Pipeline> pipeline,
        boost::optional<const AggregateCommandRequest&> aggRequest,
        bool shouldUseCollectionDefaultCollator) override {
        pipeline->addInitialSource(
            DocumentSourceMock::createForTest(_mockResults, pipeline->getContext()));
        return pipeline;
    }

private:
    std::deque<DocumentSource::GetNextResult> _mockResults;
};

repl::MutableOplogEntry makeOplog(const NamespaceString& nss,
                                  const Timestamp& timestamp,
                                  const UUID& uuid,
                                  const ShardId& shardId,
                                  const repl::OpTypeEnum& opType,
                                  const BSONObj& oField,
                                  const BSONObj& o2Field,
                                  const boost::optional<ReshardingDonorOplogId>& _id) {
    repl::MutableOplogEntry oplogEntry;
    oplogEntry.setNss(nss);
    oplogEntry.setTimestamp(timestamp);
    oplogEntry.setWallClockTime(Date_t::now());
    oplogEntry.setTerm(1);
    oplogEntry.setUuid(uuid);
    oplogEntry.setOpType(opType);
    oplogEntry.setObject(oField);

    if (!o2Field.isEmpty()) {
        oplogEntry.setObject2(o2Field);
    }

    if (shardId.isValid()) {
        oplogEntry.setDestinedRecipient(shardId);
    }

    oplogEntry.set_id(Value(_id->toBSON()));

    return oplogEntry;
}

class MockedMongoInterfaceFactory : public MongoProcessInterfaceFactory {
public:
    MockedMongoInterfaceFactory(std::shared_ptr<MockMongoInterface> mockMongoInterface)
        : _mockMongoInterface(std::move(mockMongoInterface)) {}

    std::shared_ptr<MongoProcessInterface> create(OperationContext* opCtx) override {
        return _mockMongoInterface;
    }

private:
    std::shared_ptr<MockMongoInterface> _mockMongoInterface;
};

class ReshardingDonorOplogPipelineTest : public ServiceContextTest {
protected:
    const size_t kBatchLimit = 100;
    const ReshardingDonorOplogId kDefaultResumeToken = {Timestamp::min(), Timestamp::min()};
    const NamespaceString& localOplogBufferNss() {
        return kLocalOplogBufferNss;
    }

    auto makePipelineForReshardingDonorOplogIterator(
        std::deque<DocumentSource::GetNextResult> mockResults) {
        auto mockMongoInterface = std::make_shared<MockMongoInterface>(mockResults);
        return ReshardingDonorOplogPipeline(
            localOplogBufferNss(),
            std::make_unique<MockedMongoInterfaceFactory>(mockMongoInterface));
    }

    /************************************************************************************
     * These set of helper function generate pre-made oplogs with the following timestamps:
     *
     * deletePreImage: ts(7, 35)
     * updatePostImage: ts(10, 15)
     * insert: ts(25, 345)
     * update: ts(30, 16)
     * delete: ts(66, 86)
     */

    repl::MutableOplogEntry makeInsertOplog() {
        const Timestamp insertTs(25, 345);
        const ReshardingDonorOplogId insertId(insertTs, insertTs);
        return makeOplog(_crudNss,
                         insertTs,
                         _reshardingCollUUID,
                         _destinedRecipient,
                         repl::OpTypeEnum::kInsert,
                         BSON("x" << 1),
                         {},
                         insertId);
    }

    repl::MutableOplogEntry makeUpdateOplog() {
        const Timestamp updateWithPostOplogTs(30, 16);
        const ReshardingDonorOplogId updateWithPostOplogId(updateWithPostOplogTs,
                                                           updateWithPostOplogTs);
        return makeOplog(_crudNss,
                         updateWithPostOplogTs,
                         _reshardingCollUUID,
                         _destinedRecipient,
                         repl::OpTypeEnum::kUpdate,
                         BSON("$set" << BSON("y" << 1)),
                         BSON("post" << 1),
                         updateWithPostOplogId);
    }

    repl::MutableOplogEntry makeDeleteOplog() {
        const Timestamp deleteWithPreOplogTs(66, 86);
        const ReshardingDonorOplogId deleteWithPreOplogId(deleteWithPreOplogTs,
                                                          deleteWithPreOplogTs);
        return makeOplog(_crudNss,
                         deleteWithPreOplogTs,
                         _reshardingCollUUID,
                         _destinedRecipient,
                         repl::OpTypeEnum::kDelete,
                         BSON("pre" << 1),
                         {},
                         deleteWithPreOplogId);
    }


    ReshardingDonorOplogId getOplogId(const repl::MutableOplogEntry& oplog) {
        return ReshardingDonorOplogId::parse(oplog.get_id()->getDocument().toBson(),
                                             IDLParserContext("ReshardingAggTest::getOplogId"));
    }

    ReshardingDonorOplogId getOplogId(const repl::OplogEntry& oplog) {
        return ReshardingDonorOplogId::parse(oplog.getEntry().get_id()->getDocument().toBson(),
                                             IDLParserContext("ReshardingAggTest::getOplogId"));
    }

    const NamespaceString _crudNss = NamespaceString::createNamespaceString_forTest("test.foo");
    // Use a constant value so unittests can store oplog entries as extended json strings in code.
    const UUID _reshardingCollUUID =
        fassert(11625800, UUID::parse("8926ba8e-611a-42c2-bb1a-3b7819f610ed"));
    // Also referenced via strings in code.
    const ShardId _destinedRecipient = {"shard1"};
};

using ReshardingDonorOplogPipelineDeathTest = ReshardingDonorOplogPipelineTest;

TEST_F(ReshardingDonorOplogPipelineTest, OplogPipelineBasicCRUDOnly) {
    auto insertOplog = makeInsertOplog();
    auto updateOplog = makeUpdateOplog();
    auto deleteOplog = makeDeleteOplog();

    std::deque<DocumentSource::GetNextResult> mockResults;
    mockResults.emplace_back(Document(insertOplog.toBSON()));
    mockResults.emplace_back(Document(updateOplog.toBSON()));
    mockResults.emplace_back(Document(deleteOplog.toBSON()));

    auto opCtx = makeOperationContext();
    auto pipeline = makePipelineForReshardingDonorOplogIterator(std::move(mockResults));
    auto scopedPipeline = pipeline.initWithOperationContext(opCtx.get(), kDefaultResumeToken);

    auto batch = scopedPipeline.getNextBatch(kBatchLimit);
    ASSERT_EQ(3U, batch.size());

    ASSERT_BSONOBJ_BINARY_EQ(insertOplog.toBSON(), batch[0].getEntry().toBSON());
    ASSERT_BSONOBJ_BINARY_EQ(updateOplog.toBSON(), batch[1].getEntry().toBSON());
    ASSERT_BSONOBJ_BINARY_EQ(deleteOplog.toBSON(), batch[2].getEntry().toBSON());
}

TEST_F(ReshardingDonorOplogPipelineTest, SmallBatchLimit) {
    auto insertOplog = makeInsertOplog();
    auto updateOplog = makeUpdateOplog();
    auto deleteOplog = makeDeleteOplog();

    std::deque<DocumentSource::GetNextResult> mockResults;
    mockResults.emplace_back(Document(insertOplog.toBSON()));
    mockResults.emplace_back(Document(updateOplog.toBSON()));
    mockResults.emplace_back(Document(deleteOplog.toBSON()));

    auto opCtx = makeOperationContext();
    auto pipeline = makePipelineForReshardingDonorOplogIterator(std::move(mockResults));
    auto scopedPipeline = pipeline.initWithOperationContext(opCtx.get(), kDefaultResumeToken);

    auto firstBatch = scopedPipeline.getNextBatch(2);
    ASSERT_EQ(2U, firstBatch.size());

    ASSERT_BSONOBJ_BINARY_EQ(insertOplog.toBSON(), firstBatch[0].getEntry().toBSON());
    ASSERT_BSONOBJ_BINARY_EQ(updateOplog.toBSON(), firstBatch[1].getEntry().toBSON());

    auto secondBatch = scopedPipeline.getNextBatch(2);
    ASSERT_EQ(1U, secondBatch.size());
    ASSERT_BSONOBJ_BINARY_EQ(deleteOplog.toBSON(), secondBatch[0].getEntry().toBSON());
}

TEST_F(ReshardingDonorOplogPipelineTest, ShouldBeAbleToResumeAfterDetach) {
    auto insertOplog = makeInsertOplog();
    auto updateOplog = makeUpdateOplog();
    auto deleteOplog = makeDeleteOplog();

    std::deque<DocumentSource::GetNextResult> mockResults;
    mockResults.emplace_back(Document(insertOplog.toBSON()));
    mockResults.emplace_back(Document(updateOplog.toBSON()));
    mockResults.emplace_back(Document(deleteOplog.toBSON()));

    auto pipeline = makePipelineForReshardingDonorOplogIterator(std::move(mockResults));

    {
        auto opCtx = makeOperationContext();
        auto scopedPipeline = pipeline.initWithOperationContext(opCtx.get(), kDefaultResumeToken);

        auto firstBatch = scopedPipeline.getNextBatch(2);
        ASSERT_EQ(2U, firstBatch.size());

        ASSERT_BSONOBJ_BINARY_EQ(insertOplog.toBSON(), firstBatch[0].getEntry().toBSON());
        ASSERT_BSONOBJ_BINARY_EQ(updateOplog.toBSON(), firstBatch[1].getEntry().toBSON());

        scopedPipeline.detachFromOperationContext();
    }

    {
        // ResumeToken is ignored because this will reattach to an active pipeline since we
        // detached cleanly last time.
        auto opCtx = makeOperationContext();
        auto scopedPipeline = pipeline.initWithOperationContext(opCtx.get(), kDefaultResumeToken);

        auto secondBatch = scopedPipeline.getNextBatch(2);
        ASSERT_EQ(1U, secondBatch.size());
        ASSERT_BSONOBJ_BINARY_EQ(deleteOplog.toBSON(), secondBatch[0].getEntry().toBSON());
    }
}

TEST_F(ReshardingDonorOplogPipelineTest, ShouldBeAbleToResumeWithoutDetach) {
    auto insertOplog = makeInsertOplog();
    auto updateOplog = makeUpdateOplog();
    auto deleteOplog = makeDeleteOplog();

    std::deque<DocumentSource::GetNextResult> mockResults;
    mockResults.emplace_back(Document(insertOplog.toBSON()));
    mockResults.emplace_back(Document(updateOplog.toBSON()));
    mockResults.emplace_back(Document(deleteOplog.toBSON()));

    auto pipeline = makePipelineForReshardingDonorOplogIterator(std::move(mockResults));
    auto resumeToken = ([&]() {
        auto opCtx = makeOperationContext();
        auto scopedPipeline = pipeline.initWithOperationContext(opCtx.get(), kDefaultResumeToken);

        auto firstBatch = scopedPipeline.getNextBatch(2);
        ASSERT_EQ(2U, firstBatch.size());

        ASSERT_BSONOBJ_BINARY_EQ(insertOplog.toBSON(), firstBatch[0].getEntry().toBSON());
        ASSERT_BSONOBJ_BINARY_EQ(updateOplog.toBSON(), firstBatch[1].getEntry().toBSON());
        return getOplogId(firstBatch[1]);
    })();

    {
        auto opCtx = makeOperationContext();
        // This will re-initialize the pipeline since we didn't detach cleanly last time.
        auto scopedPipeline = pipeline.initWithOperationContext(opCtx.get(), resumeToken);

        auto secondBatch = scopedPipeline.getNextBatch(2);
        ASSERT_EQ(1U, secondBatch.size());
        ASSERT_BSONOBJ_BINARY_EQ(deleteOplog.toBSON(), secondBatch[0].getEntry().toBSON());
    }

    {
        auto opCtx = makeOperationContext();
        // This will re-initialize the pipeline for the second time since we didn't detach
        // cleanly last time.
        auto scopedPipeline = pipeline.initWithOperationContext(opCtx.get(), resumeToken);

        auto secondBatch = scopedPipeline.getNextBatch(2);
        ASSERT_EQ(1U, secondBatch.size());
        ASSERT_BSONOBJ_BINARY_EQ(deleteOplog.toBSON(), secondBatch[0].getEntry().toBSON());
    }
}

DEATH_TEST_F(ReshardingDonorOplogPipelineDeathTest,
             CannotCallGetNextWithoutProperlyReattaching,
             "Invariant failure") {
    auto insertOplog = makeInsertOplog();
    auto updateOplog = makeUpdateOplog();
    auto deleteOplog = makeDeleteOplog();

    std::deque<DocumentSource::GetNextResult> mockResults;
    mockResults.emplace_back(Document(insertOplog.toBSON()));
    mockResults.emplace_back(Document(updateOplog.toBSON()));
    mockResults.emplace_back(Document(deleteOplog.toBSON()));

    auto pipeline = makePipelineForReshardingDonorOplogIterator(std::move(mockResults));

    {
        auto opCtx = makeOperationContext();
        auto scopedPipeline = pipeline.initWithOperationContext(opCtx.get(), kDefaultResumeToken);

        auto firstBatch = scopedPipeline.getNextBatch(2);
        ASSERT_EQ(2U, firstBatch.size());

        ASSERT_BSONOBJ_BINARY_EQ(insertOplog.toBSON(), firstBatch[0].getEntry().toBSON());
        ASSERT_BSONOBJ_BINARY_EQ(updateOplog.toBSON(), firstBatch[1].getEntry().toBSON());

        scopedPipeline.detachFromOperationContext();

        (void)scopedPipeline.getNextBatch(2);
    }
}

/**
 * Test with 3 oplog: insert -> update -> delete, then resume from point after insert.
 */
TEST_F(ReshardingDonorOplogPipelineTest, OplogPipelineWithResumeToken) {
    auto insertOplog = makeInsertOplog();
    auto updateOplog = makeUpdateOplog();
    auto deleteOplog = makeDeleteOplog();

    std::deque<DocumentSource::GetNextResult> mockResults;
    mockResults.emplace_back(Document(insertOplog.toBSON()));
    mockResults.emplace_back(Document(updateOplog.toBSON()));
    mockResults.emplace_back(Document(deleteOplog.toBSON()));

    auto opCtx = makeOperationContext();
    auto pipeline = makePipelineForReshardingDonorOplogIterator(std::move(mockResults));
    auto scopedPipeline = pipeline.initWithOperationContext(opCtx.get(), getOplogId(insertOplog));

    auto batch = scopedPipeline.getNextBatch(kBatchLimit);
    ASSERT_EQ(2U, batch.size());

    ASSERT_BSONOBJ_BINARY_EQ((updateOplog.toBSON()), batch[0].getEntry().toBSON());
    ASSERT_BSONOBJ_BINARY_EQ(deleteOplog.toBSON(), batch[1].getEntry().toBSON());
}

/**
 * Test with 3 oplog: insert -> update -> delete, then resume from point after insert.
 * In this test, the clusterTime portion of the oplogId has a different timestamp to the
 * ts portion of oplogId.
 */
TEST_F(ReshardingDonorOplogPipelineTest, OplogPipelineWithResumeTokenClusterTimeNotEqualTs) {
    auto modifyClusterTsTo = [&](repl::MutableOplogEntry& oplog, const Timestamp& ts) {
        auto newId = getOplogId(oplog);
        newId.setClusterTime(ts);
        oplog.set_id(Value(newId.toBSON()));
    };

    auto insertOplog = makeInsertOplog();
    modifyClusterTsTo(insertOplog, Timestamp(33, 46));
    auto updateOplog = makeUpdateOplog();
    modifyClusterTsTo(updateOplog, Timestamp(44, 55));
    auto deleteOplog = makeDeleteOplog();
    modifyClusterTsTo(deleteOplog, Timestamp(79, 80));

    std::deque<DocumentSource::GetNextResult> mockResults;
    mockResults.emplace_back(Document(insertOplog.toBSON()));
    mockResults.emplace_back(Document(updateOplog.toBSON()));
    mockResults.emplace_back(Document(deleteOplog.toBSON()));

    auto pipeline = makePipelineForReshardingDonorOplogIterator(std::move(mockResults));
    auto opCtx = makeOperationContext();
    auto scopedPipeline = pipeline.initWithOperationContext(opCtx.get(), getOplogId(insertOplog));

    auto batch = scopedPipeline.getNextBatch(kBatchLimit);
    ASSERT_EQ(2U, batch.size());

    ASSERT_BSONOBJ_BINARY_EQ(updateOplog.toBSON(), batch[0].getEntry().toBSON());
    ASSERT_BSONOBJ_BINARY_EQ(deleteOplog.toBSON(), batch[1].getEntry().toBSON());
}

}  // namespace
}  // namespace mongo
