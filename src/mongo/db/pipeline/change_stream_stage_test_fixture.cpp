/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/pipeline/change_stream_stage_test_fixture.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/agg/change_stream_transform_stage.h"
#include "mongo/db/exec/agg/pipeline_builder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/change_stream_test_helpers.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_change_stream_ensure_resume_token_present.h"
#include "mongo/db/pipeline/document_source_change_stream_transform.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/transaction/transaction_history_iterator.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <iterator>
#include <list>
#include <memory>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

namespace {
// This mock iterator simulates a traversal of transaction history in the oplog by returning
// mock oplog entries from a list.
struct MockTransactionHistoryIterator : public TransactionHistoryIteratorBase {
    bool hasNext() const final {
        return (mockEntriesIt != mockEntries.end());
    }

    repl::OplogEntry next(OperationContext* opCtx) final {
        ASSERT(hasNext());
        return *(mockEntriesIt++);
    }

    repl::OpTime nextOpTime(OperationContext* opCtx) final {
        ASSERT(hasNext());
        return (mockEntriesIt++)->getOpTime();
    }

    std::vector<repl::OplogEntry> mockEntries;
    std::vector<repl::OplogEntry>::const_iterator mockEntriesIt;
};

}  // namespace

ChangeStreamStageTestNoSetup::ChangeStreamStageTestNoSetup()
    : ChangeStreamStageTestNoSetup(change_stream_test_helper::nss) {}

ChangeStreamStageTestNoSetup::ChangeStreamStageTestNoSetup(NamespaceString nsString)
    : AggregationContextFixture(std::move(nsString)) {
    getExpCtx()->setMongoProcessInterface(std::make_unique<ExecutableStubMongoProcessInterface>());
}

const UUID& MockMongoInterface::oplogUuid() {
    static const UUID* oplog_uuid = new UUID(UUID::gen());
    return *oplog_uuid;
}

MockMongoInterface::MockMongoInterface(std::vector<repl::OplogEntry> transactionEntries,
                                       std::vector<Document> documentsForLookup)
    : _transactionEntries(std::move(transactionEntries)),
      _documentsForLookup{std::move(documentsForLookup)} {}

// For tests of transactions that involve multiple oplog entries.
std::unique_ptr<TransactionHistoryIteratorBase>
MockMongoInterface::createTransactionHistoryIterator(repl::OpTime time) const {
    auto iterator = std::make_unique<MockTransactionHistoryIterator>();

    // Simulate a lookup on the oplog timestamp by manually advancing the iterator until we
    // reach the desired timestamp.
    iterator->mockEntries = _transactionEntries;
    ASSERT(iterator->mockEntries.size() > 0);
    for (iterator->mockEntriesIt = iterator->mockEntries.begin();
         iterator->mockEntriesIt->getOpTime() != time;
         ++iterator->mockEntriesIt) {
        ASSERT(iterator->mockEntriesIt != iterator->mockEntries.end());
    }

    return iterator;
}

// Called by DocumentSourceAddPreImage to obtain the UUID of the oplog. Since that's the only
// piece of collection info we need for now, just return a BSONObj with the mock oplog UUID.
BSONObj MockMongoInterface::getCollectionOptions(OperationContext* opCtx,
                                                 const NamespaceString& nss) {
    return BSON("uuid" << oplogUuid());
}

boost::optional<Document> MockMongoInterface::lookupSingleDocument(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    boost::optional<UUID> collectionUUID,
    const Document& documentKey,
    boost::optional<BSONObj> readConcern) {
    Matcher matcher(documentKey.toBson(), expCtx);
    auto it = std::find_if(
        _documentsForLookup.begin(), _documentsForLookup.end(), [&](const Document& lookedUpDoc) {
            return exec::matcher::matches(&matcher, lookedUpDoc.toBson(), nullptr);
        });
    return (it != _documentsForLookup.end() ? *it : boost::optional<Document>{});
}

ChangeStreamStageTest::ChangeStreamStageTest()
    : ChangeStreamStageTest(change_stream_test_helper::nss) {
    // Initialize the UUID on the ExpressionContext, to allow tests with a resumeToken.
    getExpCtx()->setUUID(change_stream_test_helper::testUuid());
}

ChangeStreamStageTest::ChangeStreamStageTest(NamespaceString nsString)
    : ChangeStreamStageTestNoSetup(nsString) {
    repl::ReplicationCoordinator::set(getExpCtx()->getOperationContext()->getServiceContext(),
                                      std::make_unique<repl::ReplicationCoordinatorMock>(
                                          getExpCtx()->getOperationContext()->getServiceContext()));
}

void ChangeStreamStageTest::checkTransformation(
    const repl::OplogEntry& entry,
    const boost::optional<Document>& expectedDoc,
    const BSONObj& spec,
    const boost::optional<Document>& expectedInvalidate,
    const std::vector<repl::OplogEntry>& transactionEntries,
    std::vector<Document> documentsForLookup,
    const boost::optional<std::int32_t>& expectedErrorCode) {
    auto execPipeline = makeExecPipeline(entry.getEntry().toBSON(), spec);
    auto lastStage = execPipeline->getStages().back();

    getExpCtx()->setMongoProcessInterface(
        std::make_unique<MockMongoInterface>(transactionEntries, std::move(documentsForLookup)));

    if (expectedErrorCode) {
        ASSERT_THROWS_CODE(
            lastStage->getNext(), AssertionException, ErrorCodes::Error(expectedErrorCode.get()));
        return;
    }

    auto next = lastStage->getNext();
    // Match stage should pass the doc down if expectedDoc is given.
    ASSERT_EQ(next.isAdvanced(), static_cast<bool>(expectedDoc));
    if (expectedDoc) {
        ASSERT_DOCUMENT_EQ(next.releaseDocument(), *expectedDoc);
    }

    if (expectedInvalidate) {
        next = lastStage->getNext();
        ASSERT_TRUE(next.isAdvanced());
        ASSERT_DOCUMENT_EQ(next.releaseDocument(), *expectedInvalidate);

        // Then throw an exception on the next call of getNext().
        ASSERT_THROWS(lastStage->getNext(), ExceptionFor<ErrorCodes::ChangeStreamInvalidated>);
    }
}

/**
 * Returns a list of stages expanded from a $changStream specification, starting with a
 * DocumentSourceMock which contains a single document representing 'entry'.
 *
 * Stages such as DSEnsureResumeTokenPresent which can swallow results are removed from the
 * returned list.
 */
std::unique_ptr<exec::agg::Pipeline> ChangeStreamStageTest::makeExecPipeline(BSONObj entry,
                                                                             const BSONObj& spec) {
    return makeExecPipeline({entry}, spec, true /* removeEnsureResumeTokenStage */);
}

/**
 * Returns a list of the stages expanded from a $changStream specification, starting with a
 * DocumentSourceMock which contains a list of document representing 'entries'.
 */
std::unique_ptr<exec::agg::Pipeline> ChangeStreamStageTest::makeExecPipeline(
    std::vector<BSONObj> entries, const BSONObj& spec, bool removeEnsureResumeTokenStage) {
    std::list<boost::intrusive_ptr<DocumentSource>> result =
        DocumentSourceChangeStream::createFromBson(spec.firstElement(), getExpCtx());
    getExpCtx()->setMongoProcessInterface(std::make_unique<MockMongoInterface>());

    // This match stage is a DocumentSourceChangeStreamOplogMatch, which we explicitly disallow
    // from executing as a safety mechanism, since it needs to use the collection-default
    // collation, even if the rest of the pipeline is using some other collation. To avoid ever
    // executing that stage here, we'll up-convert it from the non-executable
    // DocumentSourceChangeStreamOplogMatch to a fully-executable DocumentSourceMatch. This is
    // safe because all of the unit tests will use the 'simple' collation.
    auto match = dynamic_cast<DocumentSourceMatch*>(result.front().get());
    ASSERT(match);
    auto executableMatch = DocumentSourceMatch::create(match->getQuery(), getExpCtx());
    // Replace the original match with the executable one.
    result.front() = executableMatch;

    // Check the oplog entry is transformed correctly.
    auto transform = std::next(result.begin(), 2)->get();
    ASSERT(transform);
    ASSERT(dynamic_cast<DocumentSourceChangeStreamTransform*>(transform));

    // Create mock stage and insert at the front of the stages.
    auto mock = DocumentSourceMock::createForTest(entries, getExpCtx());
    result.insert(result.begin(), mock);

    if (removeEnsureResumeTokenStage) {
        auto newEnd = std::remove_if(result.begin(), result.end(), [](auto& stage) {
            return dynamic_cast<DocumentSourceChangeStreamEnsureResumeTokenPresent*>(stage.get());
        });
        result.erase(newEnd, result.end());
    }

    auto pipeline = Pipeline::create(result, getExpCtx());
    return exec::agg::buildPipeline(pipeline->freeze());
}

std::unique_ptr<exec::agg::Pipeline> ChangeStreamStageTest::makeExecPipeline(
    const repl::OplogEntry& entry, const BSONObj& spec) {
    return makeExecPipeline(entry.getEntry().toBSON(), spec);
}

repl::OplogEntry ChangeStreamStageTest::createCommand(const BSONObj& oField,
                                                      boost::optional<UUID> uuid,
                                                      boost::optional<bool> fromMigrate,
                                                      boost::optional<repl::OpTime> opTime) {
    return change_stream_test_helper::makeOplogEntry(
        repl::OpTypeEnum::kCommand,                     // op type
        change_stream_test_helper::nss.getCommandNS(),  // namespace
        oField,                                         // o
        uuid,                                           // uuid
        fromMigrate,                                    // fromMigrate
        boost::none,                                    // o2
        opTime);                                        // opTime
}


/**
 * Helper for running an applyOps through the pipeline, and getting all of the results.
 */
std::vector<Document> ChangeStreamStageTest::getApplyOpsResults(
    const Document& applyOpsDoc,
    const LogicalSessionFromClient& lsid,
    BSONObj spec,
    bool hasTxnNumber) {
    BSONObj applyOpsObj = applyOpsDoc.toBson();

    // Create an oplog entry and then glue on an lsid and optionally a txnNumber
    auto baseOplogEntry =
        change_stream_test_helper::makeOplogEntry(repl::OpTypeEnum::kCommand,
                                                  change_stream_test_helper::nss.getCommandNS(),
                                                  applyOpsObj,
                                                  change_stream_test_helper::testUuid(),
                                                  boost::none,  // fromMigrate
                                                  BSONObj());
    BSONObjBuilder builder(baseOplogEntry.getEntry().toBSON());
    builder.append("lsid", lsid.toBSON());
    if (hasTxnNumber) {
        builder.append("txnNumber", 0LL);
    }
    BSONObj oplogEntry = builder.done();

    // Create the stages and check that the documents produced matched those in the applyOps.
    auto execPipeline = makeExecPipeline(oplogEntry, spec);
    auto transform = execPipeline->getStages()[3].get();
    invariant(dynamic_cast<exec::agg::ChangeStreamTransformStage*>(transform) != nullptr);

    std::vector<Document> res;
    auto next = transform->getNext();
    while (next.isAdvanced()) {
        res.push_back(next.releaseDocument());
        next = transform->getNext();
    }
    return res;
}

Document ChangeStreamStageTest::makeExpectedUpdateEvent(Timestamp ts,
                                                        const NamespaceString& nss,
                                                        BSONObj documentKey,
                                                        Document updateDescription,
                                                        bool expandedEvents) {
    return Document{
        {DocumentSourceChangeStream::kIdField,
         change_stream_test_helper::makeResumeToken(ts,
                                                    change_stream_test_helper::testUuid(),
                                                    Value{documentKey},
                                                    DocumentSourceChangeStream::kUpdateOpType)},
        {DocumentSourceChangeStream::kOperationTypeField,
         DocumentSourceChangeStream::kUpdateOpType},
        {DocumentSourceChangeStream::kClusterTimeField, ts},
        {DocumentSourceChangeStream::kCollectionUuidField,
         expandedEvents ? Value{change_stream_test_helper::testUuid()} : Value{}},
        {DocumentSourceChangeStream::kWallTimeField, Date_t()},
        {DocumentSourceChangeStream::kNamespaceField,
         Document{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
        {DocumentSourceChangeStream::kDocumentKeyField, Value{documentKey}},
        {"updateDescription", updateDescription},
    };
}

/**
 * Helper function to do a $v:2 delta oplog test.
 */
void ChangeStreamStageTest::runUpdateV2OplogTest(BSONObj diff, Document updateModificationEntry) {
    BSONObj o2 = BSON("_id" << 1);
    auto deltaOplog =
        change_stream_test_helper::makeOplogEntry(repl::OpTypeEnum::kUpdate,          // op type
                                                  change_stream_test_helper::nss,     // namespace
                                                  BSON("diff" << diff << "$v" << 2),  // o
                                                  change_stream_test_helper::testUuid(),  // uuid
                                                  boost::none,  // fromMigrate
                                                  o2);          // o2

    const auto expectedUpdateField = makeExpectedUpdateEvent(change_stream_test_helper::kDefaultTs,
                                                             change_stream_test_helper::nss,
                                                             o2,
                                                             updateModificationEntry);
    checkTransformation(deltaOplog, expectedUpdateField);
}

/**
 * Helper to create change stream pipeline for testing.
 */
std::unique_ptr<Pipeline> ChangeStreamStageTest::buildTestPipeline(
    const std::vector<BSONObj>& rawPipeline) {
    auto expCtx = getExpCtx();
    expCtx->setNamespaceString(
        NamespaceString::createNamespaceString_forTest(boost::none, "a.collection"));
    expCtx->setInRouter(true);

    auto pipeline = Pipeline::parse(rawPipeline, expCtx);
    pipeline->optimizePipeline();

    return pipeline;
}

/**
 * Helper to verify if the change stream pipeline contains expected stages.
 */
void ChangeStreamStageTest::assertStagesNameOrder(std::unique_ptr<Pipeline> pipeline,
                                                  const std::vector<std::string>& expectedStages) {
    ASSERT_EQ(pipeline->size(), expectedStages.size());

    auto stagesItr = pipeline->getSources().cbegin();
    auto expectedStagesItr = expectedStages.cbegin();

    while (expectedStagesItr != expectedStages.end()) {
        ASSERT_EQ(*expectedStagesItr, stagesItr->get()->getSourceName());
        ++expectedStagesItr;
        ++stagesItr;
    }
}

}  // namespace mongo
