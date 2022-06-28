/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include <boost/intrusive_ptr.hpp>
#include <memory>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/collection_mock.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_change_stream_check_resumability.h"
#include "mongo/db/pipeline/document_source_change_stream_ensure_resume_token_present.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/devnull/devnull_kv_engine.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

using boost::intrusive_ptr;

namespace mongo {
namespace {
static constexpr StringData kOtherNs = "test.other.ns"_sd;
static constexpr StringData kTestNs = "test.ns"_sd;

class ChangeStreamOplogCursorMock : public SeekableRecordCursor {
public:
    ChangeStreamOplogCursorMock(std::deque<Record>* records) : _records(records) {}

    virtual ~ChangeStreamOplogCursorMock() {}

    boost::optional<Record> next() override {
        if (_records->empty()) {
            return boost::none;
        }
        auto& next = _records->front();
        _records->pop_front();
        return next;
    }

    boost::optional<Record> seekExact(const RecordId& id) override {
        return Record{};
    }
    boost::optional<Record> seekNear(const RecordId& id) override {
        return boost::none;
    }
    void save() override {}
    bool restore(bool tolerateCappedRepositioning) override {
        return true;
    }
    void detachFromOperationContext() override {}
    void reattachToOperationContext(OperationContext* opCtx) override {}
    void setSaveStorageCursorOnDetachFromOperationContext(bool) {}

private:
    std::deque<Record>* _records;
};

class ChangeStreamOplogCollectionMock : public CollectionMock {
public:
    ChangeStreamOplogCollectionMock() : CollectionMock(NamespaceString::kRsOplogNamespace) {
        _recordStore =
            _devNullEngine.getRecordStore(nullptr, NamespaceString::kRsOplogNamespace, "", {});
    }

    void push_back(Document doc) {
        // Every entry we push into the oplog should have both 'ts' and 'ns' fields.
        invariant(doc["ts"].getType() == BSONType::bsonTimestamp);
        invariant(doc["ns"].getType() == BSONType::String);
        // Events should always be added in ascending ts order.
        auto lastTs =
            _records.empty() ? Timestamp(0, 0) : _records.back().data.toBson()["ts"].timestamp();
        invariant(ValueComparator().compare(Value(lastTs), doc["ts"]) <= 0);
        // Fill out remaining required fields in the oplog entry.
        MutableDocument mutableDoc{doc};
        mutableDoc.setField("op", Value("n"_sd));
        mutableDoc.setField("o", Value(Document{}));
        mutableDoc.setField("wall",
                            Value(Date_t::fromMillisSinceEpoch(doc["ts"].getTimestamp().asLL())));
        // Must remove _id since the oplog expects either no _id or an OID.
        mutableDoc.remove("_id");
        // Convert to owned BSON and create corresponding Records.
        _data.push_back(mutableDoc.freeze().toBson());
        Record record;
        record.data = {_data.back().objdata(), _data.back().objsize()};
        record.id = RecordId{doc["ts"].getTimestamp().asLL()};
        _records.push_back(std::move(record));
    }

    std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx,
                                                    bool forward) const override {
        return std::make_unique<ChangeStreamOplogCursorMock>(&_records);
    }

    RecordStore* getRecordStore() const override {
        return _recordStore.get();
    }

private:
    // We retain the owned record queue here because cursors may be destroyed and recreated.
    mutable std::deque<Record> _records;
    std::deque<BSONObj> _data;

    // These are no-op structures which are required by the CollectionScan.
    std::unique_ptr<RecordStore> _recordStore;
    DevNullKVEngine _devNullEngine;
};

/**
 * Acts as an initial source for the change stream pipeline, taking the place of DSOplogMatch. This
 * class maintains its own queue of documents added by each test, but also pushes each doc into an
 * underlying ChangeStreamOplogCollectionMock. When doGetNext() is called, it retrieves the next
 * document by pulling it from the mocked oplog collection via a CollectionScan, in order to test
 * the latter's changestream-specific functionality. The reason this class keeps its own queue in
 * addition to the ChangeStreamOplogCollectionMock is twofold:
 *
 *   - The _id must be stripped from each document before it can be added to the mocked oplog, since
 *     the _id field of the test document is a resume token but oplog entries are only permitted to
 *     have OID _ids. We therefore have to restore the _id field of the document pulled from the
 *     CollectionScan before passing it into the pipeline.
 *
 *   - The concept of GetNextResult::ReturnStatus::kPauseExecution does not exist in CollectionScan;
 *     NEED_TIME is somewhat analogous but cannot be artificially induced. For tests which exercise
 *     kPauseExecution, these events are stored only in the DocumentSourceChangeStreamMock queue
 *     with no corresponding entry in the ChangeStreamOplogCollectionMock queue.
 */
class DocumentSourceChangeStreamMock : public DocumentSourceMock {
public:
    DocumentSourceChangeStreamMock(const boost::intrusive_ptr<ExpressionContextForTest>& expCtx)
        : DocumentSourceMock({}, expCtx), _collectionPtr(&_collection) {
        _filterExpr = BSON("ns" << kTestNs);
        _filter = MatchExpressionParser::parseAndNormalize(_filterExpr, pExpCtx);
        _params.assertTsHasNotFallenOff = Timestamp(0);
        _params.shouldTrackLatestOplogTimestamp = true;
        _params.minRecord = RecordIdBound(RecordId(0));
        _params.tailable = true;
    }

    void setResumeToken(ResumeTokenData resumeToken) {
        invariant(!_collScan);
        _filterExpr = BSON("ns" << kTestNs << "ts" << BSON("$gte" << resumeToken.clusterTime));
        _filter = MatchExpressionParser::parseAndNormalize(_filterExpr, pExpCtx);
        _params.minRecord = RecordIdBound(RecordId(resumeToken.clusterTime.asLL()));
        _params.assertTsHasNotFallenOff = resumeToken.clusterTime;
    }

    void push_back(GetNextResult&& result) {
        // We should never push an explicit EOF onto the queue.
        invariant(!result.isEOF());
        // If there is a document supplied, add it to the mock collection.
        if (result.isAdvanced()) {
            _collection.push_back(result.getDocument());
        }
        // Both documents and pauses are stored in the DSMock queue.
        DocumentSourceMock::push_back(std::move(result));
    }

    void push_back(const GetNextResult& result) {
        MONGO_UNREACHABLE;
    }

    bool isPermanentlyEOF() const {
        return _collScan->getCommonStats()->isEOF;
    }

protected:
    GetNextResult doGetNext() override {
        // If this is the first call to doGetNext, we must create the COLLSCAN.
        if (!_collScan) {
            _collScan = std::make_unique<CollectionScan>(
                pExpCtx.get(), _collectionPtr, _params, &_ws, _filter.get());
            // The first call to doWork will create the cursor and return NEED_TIME. But it won't
            // actually scan any of the documents that are present in the mock cursor queue.
            ASSERT_EQ(_collScan->doWork(nullptr), PlanStage::NEED_TIME);
            ASSERT_EQ(_getNumDocsTested(), 0);
        }
        while (true) {
            // If the next result is a pause, return it and don't collscan.
            auto nextResult = DocumentSourceMock::doGetNext();
            if (nextResult.isPaused()) {
                return nextResult;
            }
            // Otherwise, retrieve the document via the CollectionScan stage.
            auto id = WorkingSet::INVALID_ID;
            switch (_collScan->doWork(&id)) {
                case PlanStage::IS_EOF:
                    invariant(nextResult.isEOF());
                    return nextResult;
                case PlanStage::ADVANCED: {
                    // We need to restore the _id field which was removed when we added this
                    // entry into the oplog. This is like a stripped-down DSCSTransform stage.
                    MutableDocument mutableDoc{_ws.get(id)->doc.value()};
                    mutableDoc["_id"] = nextResult.getDocument()["_id"];
                    return mutableDoc.freeze();
                }
                case PlanStage::NEED_TIME:
                    continue;
                case PlanStage::NEED_YIELD:
                    MONGO_UNREACHABLE;
            }
        }
        MONGO_UNREACHABLE;
    }

private:
    size_t _getNumDocsTested() {
        return static_cast<const CollectionScanStats*>(_collScan->getSpecificStats())->docsTested;
    }

    ChangeStreamOplogCollectionMock _collection;
    CollectionPtr _collectionPtr;
    std::unique_ptr<CollectionScan> _collScan;
    CollectionScanParams _params;

    std::unique_ptr<MatchExpression> _filter;
    BSONObj _filterExpr;

    WorkingSet _ws;
};

class CheckResumeTokenTest : public AggregationContextFixture {
public:
    CheckResumeTokenTest() : _mock(make_intrusive<DocumentSourceChangeStreamMock>(getExpCtx())) {}

protected:
    /**
     * Pushes a document with a resume token corresponding to the given ResumeTokenData into the
     * mock queue. This document will have an ns field that matches the test namespace, and will
     * appear in the change stream pipeline if its timestamp is at or after the resume timestamp.
     */
    void addOplogEntryOnTestNS(ResumeTokenData tokenData) {
        _mock->push_back(Document{{"ns", kTestNs},
                                  {"ts", tokenData.clusterTime},
                                  {"_id", ResumeToken(std::move(tokenData)).toDocument()}});
    }

    /**
     * Pushes a document with a resume token corresponding to the given timestamp, version,
     * txnOpIndex, docKey, and namespace into the mock queue.
     */
    void addOplogEntryOnTestNS(
        Timestamp ts, int version, std::size_t txnOpIndex, Document docKey, UUID uuid) {
        return addOplogEntryOnTestNS({ts, version, txnOpIndex, uuid, Value(docKey)});
    }

    /**
     * Pushes a document with a resume token corresponding to the given timestamp, version,
     * txnOpIndex, docKey, and namespace into the mock queue.
     */
    void addOplogEntryOnTestNS(Timestamp ts, Document docKey, UUID uuid = testUuid()) {
        addOplogEntryOnTestNS(ts, 0, 0, docKey, uuid);
    }
    /**
     * Pushes a document with a resume token corresponding to the given timestamp, _id string, and
     * namespace into the mock queue.
     */
    void addOplogEntryOnTestNS(Timestamp ts, std::string id, UUID uuid = testUuid()) {
        addOplogEntryOnTestNS(ts, 0, 0, Document{{"_id", id}}, uuid);
    }

    /**
     * Pushes a document that does not match the test namespace into the mock oplog. This will be
     * examined by the oplog CollectionScan but will not produce an event in the pipeline.
     */
    void addOplogEntryOnOtherNS(Timestamp ts) {
        _mock->push_back(Document{{"ns", kOtherNs}, {"ts", ts}});
    }

    /**
     * Pushes a pause in execution into the pipeline queue.
     */
    void addPause() {
        _mock->push_back(DocumentSource::GetNextResult::makePauseExecution());
    }

    /**
     * Convenience method to create the class under test with a given ResumeTokenData.
     */

    intrusive_ptr<DocumentSourceChangeStreamEnsureResumeTokenPresent>
    createDSEnsureResumeTokenPresent(ResumeTokenData tokenData) {
        DocumentSourceChangeStreamSpec spec;
        spec.setStartAfter(ResumeToken(tokenData));
        auto checkResumeToken =
            DocumentSourceChangeStreamEnsureResumeTokenPresent::create(getExpCtx(), spec);
        _mock->setResumeToken(std::move(tokenData));
        checkResumeToken->setSource(_mock.get());
        return checkResumeToken;
    }

    /**
     * Convenience method to create the class under test with a given timestamp, docKey, and
     * namespace.
     */
    intrusive_ptr<DocumentSourceChangeStreamEnsureResumeTokenPresent>
    createDSEnsureResumeTokenPresent(Timestamp ts,
                                     int version,
                                     std::size_t txnOpIndex,
                                     boost::optional<Document> docKey,
                                     UUID uuid) {
        return createDSEnsureResumeTokenPresent(
            {ts, version, txnOpIndex, uuid, docKey ? Value(*docKey) : Value()});
    }

    /**
     * Convenience method to create the class under test with a given timestamp, docKey, and
     * namespace.
     */
    intrusive_ptr<DocumentSourceChangeStreamEnsureResumeTokenPresent>
    createDSEnsureResumeTokenPresent(Timestamp ts,
                                     boost::optional<Document> docKey,
                                     UUID uuid = testUuid()) {
        return createDSEnsureResumeTokenPresent(ts, 0, 0, docKey, uuid);
    }

    /**
     * Convenience method to create the class under test with a given timestamp, _id string, and
     * namespace.
     */
    intrusive_ptr<DocumentSourceChangeStreamEnsureResumeTokenPresent>
    createDSEnsureResumeTokenPresent(Timestamp ts, StringData id, UUID uuid = testUuid()) {
        return createDSEnsureResumeTokenPresent(ts, 0, 0, Document{{"_id", id}}, uuid);
    }

    /**
     * This method is required to avoid a static initialization fiasco resulting from calling
     * UUID::gen() in file or class static scope.
     */
    static const UUID& testUuid() {
        static const UUID* uuid_gen = new UUID(UUID::gen());
        return *uuid_gen;
    }

    intrusive_ptr<DocumentSourceChangeStreamMock> _mock;
};

class CheckResumabilityTest : public CheckResumeTokenTest {
protected:
    intrusive_ptr<DocumentSourceChangeStreamCheckResumability> createDSCheckResumability(
        ResumeTokenData tokenData) {
        DocumentSourceChangeStreamSpec spec;
        spec.setStartAfter(ResumeToken(tokenData));
        auto dsCheckResumability =
            DocumentSourceChangeStreamCheckResumability::create(getExpCtx(), spec);
        _mock->setResumeToken(std::move(tokenData));
        dsCheckResumability->setSource(_mock.get());
        return dsCheckResumability;
    }
    intrusive_ptr<DocumentSourceChangeStreamCheckResumability> createDSCheckResumability(
        Timestamp ts) {
        return createDSCheckResumability(
            ResumeToken::makeHighWaterMarkToken(ts, ResumeTokenData::kDefaultTokenVersion)
                .getData());
    }
};

TEST_F(CheckResumeTokenTest, ShouldSucceedWithOnlyResumeToken) {
    Timestamp resumeTimestamp(100, 1);

    auto checkResumeToken = createDSEnsureResumeTokenPresent(resumeTimestamp, "1");
    addOplogEntryOnTestNS(resumeTimestamp, "1");
    // We should not see the resume token.
    ASSERT_TRUE(checkResumeToken->getNext().isEOF());
}

TEST_F(CheckResumeTokenTest, ShouldSucceedWithPausesBeforeResumeToken) {
    Timestamp resumeTimestamp(100, 1);

    auto checkResumeToken = createDSEnsureResumeTokenPresent(resumeTimestamp, "1");
    addPause();
    addOplogEntryOnTestNS(resumeTimestamp, "1");

    // We see the pause we inserted, but not the resume token.
    ASSERT_TRUE(checkResumeToken->getNext().isPaused());
    ASSERT_TRUE(checkResumeToken->getNext().isEOF());
}

TEST_F(CheckResumeTokenTest, ShouldSucceedWithPausesAfterResumeToken) {
    Timestamp resumeTimestamp(100, 1);
    Timestamp doc1Timestamp(100, 2);

    auto checkResumeToken = createDSEnsureResumeTokenPresent(resumeTimestamp, "1");
    addOplogEntryOnTestNS(resumeTimestamp, "1");
    addPause();
    addOplogEntryOnTestNS(doc1Timestamp, "2");

    // Pause added explicitly.
    ASSERT_TRUE(checkResumeToken->getNext().isPaused());
    // The document after the resume token should be the first.
    auto result1 = checkResumeToken->getNext();
    ASSERT_TRUE(result1.isAdvanced());
    auto& doc1 = result1.getDocument();
    ASSERT_EQ(doc1Timestamp, ResumeToken::parse(doc1["_id"].getDocument()).getData().clusterTime);
    ASSERT_TRUE(checkResumeToken->getNext().isEOF());
}

TEST_F(CheckResumeTokenTest, ShouldSucceedWithMultipleDocumentsAfterResumeToken) {
    Timestamp resumeTimestamp(100, 1);

    auto checkResumeToken = createDSEnsureResumeTokenPresent(resumeTimestamp, "0");
    addOplogEntryOnTestNS(resumeTimestamp, "0");

    Timestamp doc1Timestamp(100, 2);
    Timestamp doc2Timestamp(101, 1);
    addOplogEntryOnTestNS(doc1Timestamp, "1");
    addOplogEntryOnTestNS(doc2Timestamp, "2");

    auto result1 = checkResumeToken->getNext();
    ASSERT_TRUE(result1.isAdvanced());
    auto& doc1 = result1.getDocument();
    ASSERT_EQ(doc1Timestamp, ResumeToken::parse(doc1["_id"].getDocument()).getData().clusterTime);
    auto result2 = checkResumeToken->getNext();
    ASSERT_TRUE(result2.isAdvanced());
    auto& doc2 = result2.getDocument();
    ASSERT_EQ(doc2Timestamp, ResumeToken::parse(doc2["_id"].getDocument()).getData().clusterTime);
    ASSERT_TRUE(checkResumeToken->getNext().isEOF());
}

TEST_F(CheckResumeTokenTest, ShouldFailIfFirstDocHasWrongResumeToken) {
    // The first timestamp in the oplog precedes the resume token, and the second matches it...
    Timestamp doc1Timestamp(100, 1);
    Timestamp resumeTimestamp(100, 2);
    Timestamp doc2Timestamp = resumeTimestamp;

    auto checkResumeToken = createDSEnsureResumeTokenPresent(resumeTimestamp, "1");

    // ... but there's no entry in the oplog that matches the full token.
    addOplogEntryOnTestNS(doc1Timestamp, "1");
    addOplogEntryOnTestNS(doc2Timestamp, "2");
    ASSERT_THROWS_CODE(
        checkResumeToken->getNext(), AssertionException, ErrorCodes::ChangeStreamFatalError);
}

TEST_F(CheckResumeTokenTest, ShouldIgnoreChangeWithEarlierResumeToken) {
    Timestamp resumeTimestamp(100, 1);

    auto checkResumeToken = createDSEnsureResumeTokenPresent(resumeTimestamp, "1");

    // Add an entry into the oplog with the same timestamp but a lower documentKey. We swallow it
    // but don't throw - we haven't surpassed the token yet and still may see it in the next doc.
    addOplogEntryOnTestNS(resumeTimestamp, "0");
    ASSERT_TRUE(checkResumeToken->getNext().isEOF());
}

TEST_F(CheckResumeTokenTest, ShouldFailIfTokenHasWrongNamespace) {
    Timestamp resumeTimestamp(100, 1);

    auto resumeTokenUUID = UUID::gen();
    auto checkResumeToken = createDSEnsureResumeTokenPresent(resumeTimestamp, "1", resumeTokenUUID);
    auto otherUUID = UUID::gen();
    addOplogEntryOnTestNS(resumeTimestamp, "1", otherUUID);
    ASSERT_THROWS_CODE(
        checkResumeToken->getNext(), AssertionException, ErrorCodes::ChangeStreamFatalError);
}

TEST_F(CheckResumeTokenTest, ShouldSucceedWithBinaryCollation) {
    CollatorInterfaceMock collatorCompareLower(CollatorInterfaceMock::MockType::kToLowerString);
    getExpCtx()->setCollator(collatorCompareLower.clone());

    Timestamp resumeTimestamp(100, 1);

    auto checkResumeToken = createDSEnsureResumeTokenPresent(resumeTimestamp, "abc");
    // We must not see the following document.
    addOplogEntryOnTestNS(resumeTimestamp, "ABC");
    ASSERT_TRUE(checkResumeToken->getNext().isEOF());
}

TEST_F(CheckResumeTokenTest, UnshardedTokenFailsForShardedResumeOnMongosIfIdDoesNotMatchFirstDoc) {
    Timestamp resumeTimestamp(100, 1);
    getExpCtx()->inMongos = true;

    auto checkResumeToken =
        createDSEnsureResumeTokenPresent(resumeTimestamp, Document{{"_id"_sd, 1}});

    addOplogEntryOnTestNS(Timestamp(100, 1), {{"x"_sd, 0}, {"_id"_sd, 0}});
    addOplogEntryOnTestNS(Timestamp(100, 2), {{"x"_sd, 0}, {"_id"_sd, 2}});

    ASSERT_THROWS_CODE(
        checkResumeToken->getNext(), AssertionException, ErrorCodes::ChangeStreamFatalError);
}

TEST_F(CheckResumeTokenTest, ShardedResumeFailsOnMongosIfTokenHasSubsetOfDocumentKeyFields) {
    // Verify that the relaxed _id check only applies if _id is the sole field present in the
    // client's resume token, even if all the fields that are present match the first doc. We set
    // 'inMongos' since this is only applicable when DSCSEnsureResumeTokenPresent is running on
    // mongoS.
    Timestamp resumeTimestamp(100, 1);
    getExpCtx()->inMongos = true;

    auto checkResumeToken =
        createDSEnsureResumeTokenPresent(resumeTimestamp, Document{{"x"_sd, 0}, {"_id"_sd, 1}});

    addOplogEntryOnTestNS(Timestamp(100, 1), {{"x"_sd, 0}, {"y"_sd, -1}, {"_id"_sd, 1}});
    addOplogEntryOnTestNS(Timestamp(100, 2), {{"x"_sd, 0}, {"y"_sd, -1}, {"_id"_sd, 2}});

    ASSERT_THROWS_CODE(
        checkResumeToken->getNext(), AssertionException, ErrorCodes::ChangeStreamFatalError);
}

TEST_F(CheckResumeTokenTest, ShardedResumeFailsOnMongosIfDocumentKeyIsNonObject) {
    // Verify that a resume token whose documentKey is not a valid object will neither succeed nor
    // cause an invariant when we perform the relaxed eventIdentifier._id check when running in a
    // sharded context.
    Timestamp resumeTimestamp(100, 1);
    getExpCtx()->inMongos = true;

    auto checkResumeToken = createDSEnsureResumeTokenPresent(resumeTimestamp, boost::none);

    addOplogEntryOnTestNS(Timestamp(100, 1), {{"x"_sd, 0}, {"_id"_sd, 1}});
    addOplogEntryOnTestNS(Timestamp(100, 2), {{"x"_sd, 0}, {"_id"_sd, 2}});

    ASSERT_THROWS_CODE(
        checkResumeToken->getNext(), AssertionException, ErrorCodes::ChangeStreamFatalError);
}

TEST_F(CheckResumeTokenTest, ShardedResumeFailsOnMongosIfDocumentKeyOmitsId) {
    // Verify that a resume token whose documentKey omits the _id field will neither succeed nor
    // cause an invariant when we perform the relaxed eventIdentifier._id, even when compared
    // against an artificial stream token whose _id is also missing.
    Timestamp resumeTimestamp(100, 1);
    getExpCtx()->inMongos = true;

    auto checkResumeToken =
        createDSEnsureResumeTokenPresent(resumeTimestamp, Document{{"x"_sd, 0}});

    addOplogEntryOnTestNS(Timestamp(100, 1), {{"x"_sd, 0}, {"y"_sd, -1}, {"_id", 1}});
    addOplogEntryOnTestNS(Timestamp(100, 1), {{"x"_sd, 0}, {"y"_sd, -1}});
    addOplogEntryOnTestNS(Timestamp(100, 2), {{"x"_sd, 0}, {"y"_sd, -1}});

    ASSERT_THROWS_CODE(
        checkResumeToken->getNext(), AssertionException, ErrorCodes::ChangeStreamFatalError);
}

TEST_F(CheckResumeTokenTest,
       ShardedResumeSucceedsOnMongosWithSameClusterTimeIfUUIDsSortBeforeResumeToken) {
    // On a sharded cluster, the documents observed by the pipeline during a resume attempt may have
    // the same clusterTime if they come from different shards. If this is a whole-db or
    // cluster-wide changeStream, then their UUIDs may legitimately differ. As long as the UUID of
    // the current document sorts before the client's resume token, we should continue to examine
    // the next document in the stream.
    Timestamp resumeTimestamp(100, 1);
    getExpCtx()->inMongos = true;

    // Create an ordered array of 2 UUIDs.
    UUID uuids[2] = {UUID::gen(), UUID::gen()};

    if (uuids[0] > uuids[1]) {
        std::swap(uuids[0], uuids[1]);
    }

    // Create the resume token using the higher-sorting UUID.
    auto checkResumeToken =
        createDSEnsureResumeTokenPresent(resumeTimestamp, Document{{"_id"_sd, 1}}, uuids[1]);

    // Add two documents which have the same clusterTime but a lower UUID. One of the documents has
    // a lower docKey than the resume token, the other has a higher docKey; this demonstrates that
    // the UUID is the discriminating factor.
    addOplogEntryOnTestNS(resumeTimestamp, {{"_id"_sd, 0}}, uuids[0]);
    addOplogEntryOnTestNS(resumeTimestamp, {{"_id"_sd, 2}}, uuids[0]);

    // Add a third document that matches the resume token.
    addOplogEntryOnTestNS(resumeTimestamp, {{"_id"_sd, 1}}, uuids[1]);

    // Add a fourth document with the same timestamp and UUID whose docKey sorts after the token.
    auto expectedDocKey = Document{{"_id"_sd, 3}};
    addOplogEntryOnTestNS(resumeTimestamp, expectedDocKey, uuids[1]);

    // We should skip the first two docs, swallow the resume token, and return the fourth doc.
    const auto firstDocAfterResume = checkResumeToken->getNext();
    const auto tokenFromFirstDocAfterResume =
        ResumeToken::parse(firstDocAfterResume.getDocument()["_id"].getDocument()).getData();

    ASSERT_EQ(tokenFromFirstDocAfterResume.clusterTime, resumeTimestamp);
    ASSERT_DOCUMENT_EQ(tokenFromFirstDocAfterResume.eventIdentifier.getDocument(), expectedDocKey);
}

TEST_F(CheckResumeTokenTest,
       ShardedResumeFailsOnMongosWithSameClusterTimeIfUUIDsSortAfterResumeToken) {
    Timestamp resumeTimestamp(100, 1);
    getExpCtx()->inMongos = true;

    // Create an ordered array of 2 UUIDs.
    UUID uuids[2] = {UUID::gen(), UUID::gen()};

    if (uuids[0] > uuids[1]) {
        std::swap(uuids[0], uuids[1]);
    }

    // Create the resume token using the lower-sorting UUID.
    auto checkResumeToken =
        createDSEnsureResumeTokenPresent(resumeTimestamp, Document{{"_id"_sd, 1}}, uuids[0]);

    // Add a document which has the same clusterTime and a lower docKey but a higher UUID, followed
    // by a document which matches the resume token. This is not possible in practice, but it serves
    // to demonstrate that the resume attempt fails even when the resume token is present.
    addOplogEntryOnTestNS(resumeTimestamp, {{"_id"_sd, 0}}, uuids[1]);
    addOplogEntryOnTestNS(resumeTimestamp, {{"_id"_sd, 1}}, uuids[0]);

    ASSERT_THROWS_CODE(
        checkResumeToken->getNext(), AssertionException, ErrorCodes::ChangeStreamFatalError);
}

TEST_F(CheckResumeTokenTest, ShouldSwallowInvalidateFromEachShardForStartAfterInvalidate) {
    Timestamp resumeTimestamp(100, 1);
    Timestamp firstEventAfter(100, 2);

    // Create an array of 2 UUIDs. The first represents the UUID of the namespace before it was
    // dropped. The second is the UUID of the collection after it is recreated.
    UUID uuids[2] = {UUID::gen(), UUID::gen()};

    // This behaviour is only relevant when DSEnsureResumeTokenPresent is running on mongoS.
    getExpCtx()->inMongos = true;

    // Create a resume token representing an 'invalidate' event, and use it to seed the stage. A
    // resume token with {fromInvalidate:true} can only be used with startAfter, to start a new
    // stream after the old stream is invalidated.
    auto eventIdentifier = Value{Document{{"operationType", "drop"_sd}}};
    ResumeTokenData invalidateToken{resumeTimestamp,
                                    ResumeTokenData::kDefaultTokenVersion,
                                    /* txnOpIndex */ 0,
                                    uuids[0],
                                    std::move(eventIdentifier),
                                    ResumeTokenData::kFromInvalidate};
    auto checkResumeToken = createDSEnsureResumeTokenPresent(invalidateToken);

    // Add three documents which each have the invalidate resume token. We expect to see this in the
    // event that we are starting after an invalidate and the invalidating event occurred on several
    // shards at the same clusterTime.
    addOplogEntryOnTestNS(invalidateToken);
    addOplogEntryOnTestNS(invalidateToken);
    addOplogEntryOnTestNS(invalidateToken);

    // Add a document representing an insert which recreated the collection after it was dropped.
    auto expectedDocKey = Document{{"_id"_sd, 1}};
    addOplogEntryOnTestNS(Timestamp{100, 2}, expectedDocKey, uuids[1]);

    // DSEnsureResumeTokenPresent should confirm that the invalidate event is present, swallow it
    // and the next two invalidates, and return the insert event after the collection drop.
    const auto firstDocAfterResume = checkResumeToken->getNext();
    const auto tokenFromFirstDocAfterResume =
        ResumeToken::parse(firstDocAfterResume.getDocument()["_id"].getDocument()).getData();

    ASSERT_EQ(tokenFromFirstDocAfterResume.clusterTime, firstEventAfter);
    ASSERT_DOCUMENT_EQ(tokenFromFirstDocAfterResume.eventIdentifier.getDocument(), expectedDocKey);
}

TEST_F(CheckResumeTokenTest, ShouldNotSwallowUnrelatedInvalidateForStartAfterInvalidate) {
    Timestamp resumeTimestamp(100, 1);

    // This behaviour is only relevant when DSEnsureResumeTokenPresent is running on mongoS.
    getExpCtx()->inMongos = true;

    // Create an ordered array of of 2 UUIDs.
    std::vector<UUID> uuids = {UUID::gen(), UUID::gen()};
    std::sort(uuids.begin(), uuids.end());

    // Create a resume token representing an 'invalidate' event, and use it to seed the stage. A
    // resume token with {fromInvalidate:true} can only be used with startAfter, to start a new
    // stream after the old stream is invalidated.
    auto eventIdentifier = Value{Document{{"operationType", "drop"_sd}}};
    ResumeTokenData invalidateToken{resumeTimestamp,
                                    ResumeTokenData::kDefaultTokenVersion,
                                    /* txnOpIndex */ 0,
                                    uuids[0],
                                    eventIdentifier,
                                    ResumeTokenData::kFromInvalidate};
    auto checkResumeToken = createDSEnsureResumeTokenPresent(invalidateToken);

    // Create a second invalidate token with the same clusterTime but a different UUID.
    auto unrelatedInvalidateToken = invalidateToken;
    unrelatedInvalidateToken.uuid = uuids[1];

    // Add three documents which each have the invalidate resume token. We expect to see this in the
    // event that we are starting after an invalidate and the invalidating event occurred on several
    // shards at the same clusterTime.
    addOplogEntryOnTestNS(invalidateToken);
    addOplogEntryOnTestNS(invalidateToken);
    addOplogEntryOnTestNS(invalidateToken);

    // Add a fourth document which has the unrelated invalidate at the same clusterTime.
    addOplogEntryOnTestNS(unrelatedInvalidateToken);

    // DSEnsureResumeTokenPresent should confirm that the invalidate event is present, swallow it
    // and the next two invalidates, but decline to swallow the unrelated invalidate.
    const auto firstDocAfterResume = checkResumeToken->getNext();
    const auto tokenFromFirstDocAfterResume =
        ResumeToken::parse(firstDocAfterResume.getDocument()["_id"].getDocument()).getData();

    ASSERT_EQ(tokenFromFirstDocAfterResume, unrelatedInvalidateToken);
}

TEST_F(CheckResumeTokenTest, ShouldSkipResumeTokensWithEarlierTxnOpIndex) {
    Timestamp resumeTimestamp(100, 1);

    // Create an ordered array of 3 UUIDs.
    std::vector<UUID> uuids = {UUID::gen(), UUID::gen(), UUID::gen()};

    std::sort(uuids.begin(), uuids.end());

    auto checkResumeToken =
        createDSEnsureResumeTokenPresent(resumeTimestamp, 0, 2, Document{{"_id"_sd, 1}}, uuids[1]);

    // Add two documents which have the same clusterTime and version but a lower applyOps index. One
    // of the documents has a lower uuid than the resume token, the other has a higher uuid; this
    // demonstrates that the applyOps index is the discriminating factor.
    addOplogEntryOnTestNS(resumeTimestamp, 0, 0, {{"_id"_sd, 0}}, uuids[0]);
    addOplogEntryOnTestNS(resumeTimestamp, 0, 1, {{"_id"_sd, 2}}, uuids[2]);

    // Add a third document that matches the resume token.
    addOplogEntryOnTestNS(resumeTimestamp, 0, 2, {{"_id"_sd, 1}}, uuids[1]);

    // Add a fourth document with the same timestamp and version whose applyOps sorts after the
    // resume token.
    auto expectedDocKey = Document{{"_id"_sd, 3}};
    addOplogEntryOnTestNS(resumeTimestamp, 0, 3, expectedDocKey, uuids[1]);

    // We should skip the first two docs, swallow the resume token, and return the fourth doc.
    const auto firstDocAfterResume = checkResumeToken->getNext();
    const auto tokenFromFirstDocAfterResume =
        ResumeToken::parse(firstDocAfterResume.getDocument()["_id"].getDocument()).getData();

    ASSERT_EQ(tokenFromFirstDocAfterResume.clusterTime, resumeTimestamp);
    ASSERT_DOCUMENT_EQ(tokenFromFirstDocAfterResume.eventIdentifier.getDocument(), expectedDocKey);
}

/**
 * We should _error_ on the no-document case, because that means the resume token was not found.
 */
TEST_F(CheckResumeTokenTest, ShouldSucceedWithNoDocuments) {
    Timestamp resumeTimestamp(100, 1);

    auto checkResumeToken = createDSEnsureResumeTokenPresent(resumeTimestamp, "0");
    ASSERT_TRUE(checkResumeToken->getNext().isEOF());
}

TEST_F(CheckResumabilityTest, ShouldSucceedIfResumeTokenIsPresentAndEarliestOplogEntryBeforeToken) {
    Timestamp oplogTimestamp(100, 1);
    Timestamp resumeTimestamp(100, 2);

    auto dsCheckResumability = createDSCheckResumability(resumeTimestamp);
    addOplogEntryOnOtherNS(oplogTimestamp);
    addOplogEntryOnTestNS(resumeTimestamp, "ID");
    // We should see the resume token.
    auto result = dsCheckResumability->getNext();
    ASSERT_TRUE(result.isAdvanced());
    auto& doc = result.getDocument();
    ASSERT_EQ(resumeTimestamp, ResumeToken::parse(doc["_id"].getDocument()).getData().clusterTime);
}

TEST_F(CheckResumabilityTest,
       ShouldSucceedIfResumeTokenIsPresentAndEarliestOplogEntryEqualToToken) {
    Timestamp resumeTimestamp(100, 1);
    Timestamp oplogTimestamp(100, 1);

    auto dsCheckResumability = createDSCheckResumability(resumeTimestamp);
    addOplogEntryOnOtherNS(oplogTimestamp);
    addOplogEntryOnTestNS(resumeTimestamp, "ID");
    // We should see the resume token.
    auto result = dsCheckResumability->getNext();
    ASSERT_TRUE(result.isAdvanced());
    auto& doc = result.getDocument();
    ASSERT_EQ(resumeTimestamp, ResumeToken::parse(doc["_id"].getDocument()).getData().clusterTime);
}

TEST_F(CheckResumabilityTest, ShouldPermanentlyEofIfOplogIsEmpty) {
    Timestamp resumeTimestamp(100, 1);

    // As with other tailable cursors, starting a change stream on an empty capped collection will
    // cause the cursor to immediately and permanently EOF. This should never happen in practice,
    // since a replset member can only accept requests while in PRIMARY, SECONDARY or RECOVERING
    // states, and there must be at least one entry in the oplog in order to reach those states.
    auto shardCheckResumability = createDSCheckResumability(resumeTimestamp);
    auto result = shardCheckResumability->getNext();
    ASSERT_TRUE(result.isEOF());
    ASSERT_TRUE(_mock->isPermanentlyEOF());
}

TEST_F(CheckResumabilityTest,
       ShouldSucceedWithNoDocumentsInPipelineAndEarliestOplogEntryBeforeToken) {
    Timestamp oplogTimestamp(100, 1);
    Timestamp resumeTimestamp(100, 2);

    auto dsCheckResumability = createDSCheckResumability(resumeTimestamp);
    addOplogEntryOnOtherNS(oplogTimestamp);
    auto result = dsCheckResumability->getNext();
    ASSERT_TRUE(result.isEOF());
}

TEST_F(CheckResumabilityTest,
       ShouldSucceedWithNoDocumentsInPipelineAndEarliestOplogEntryEqualToToken) {
    Timestamp oplogTimestamp(100, 1);
    Timestamp resumeTimestamp(100, 1);

    auto dsCheckResumability = createDSCheckResumability(resumeTimestamp);
    addOplogEntryOnOtherNS(oplogTimestamp);
    auto result = dsCheckResumability->getNext();
    ASSERT_TRUE(result.isEOF());
}

TEST_F(CheckResumabilityTest, ShouldFailWithNoDocumentsInPipelineAndEarliestOplogEntryAfterToken) {
    Timestamp resumeTimestamp(100, 1);
    Timestamp oplogTimestamp(100, 2);

    auto dsCheckResumability = createDSCheckResumability(resumeTimestamp);
    addOplogEntryOnOtherNS(oplogTimestamp);
    ASSERT_THROWS_CODE(
        dsCheckResumability->getNext(), AssertionException, ErrorCodes::ChangeStreamHistoryLost);
}

TEST_F(CheckResumabilityTest, ShouldSucceedWithNoDocumentsInPipelineAndOplogIsEmpty) {
    Timestamp resumeTimestamp(100, 2);

    auto dsCheckResumability = createDSCheckResumability(resumeTimestamp);
    auto result = dsCheckResumability->getNext();
    ASSERT_TRUE(result.isEOF());
}

TEST_F(CheckResumabilityTest,
       ShouldSucceedWithLaterDocumentsInPipelineAndEarliestOplogEntryBeforeToken) {
    Timestamp oplogTimestamp(100, 1);
    Timestamp resumeTimestamp(100, 2);
    Timestamp docTimestamp(100, 3);

    auto dsCheckResumability = createDSCheckResumability(resumeTimestamp);
    addOplogEntryOnOtherNS(oplogTimestamp);
    addOplogEntryOnTestNS(docTimestamp, "ID");
    auto result = dsCheckResumability->getNext();
    ASSERT_TRUE(result.isAdvanced());
    auto& doc = result.getDocument();
    ASSERT_EQ(docTimestamp, ResumeToken::parse(doc["_id"].getDocument()).getData().clusterTime);
}

TEST_F(CheckResumabilityTest,
       ShouldSucceedWithLaterDocumentsInPipelineAndEarliestOplogEntryEqualToToken) {
    Timestamp oplogTimestamp(100, 1);
    Timestamp resumeTimestamp(100, 1);
    Timestamp docTimestamp(100, 3);

    auto dsCheckResumability = createDSCheckResumability(resumeTimestamp);
    addOplogEntryOnOtherNS(oplogTimestamp);
    addOplogEntryOnTestNS(docTimestamp, "ID");
    auto result = dsCheckResumability->getNext();
    ASSERT_TRUE(result.isAdvanced());
    auto& doc = result.getDocument();
    ASSERT_EQ(docTimestamp, ResumeToken::parse(doc["_id"].getDocument()).getData().clusterTime);
}

TEST_F(CheckResumabilityTest,
       ShouldFailWithLaterDocumentsInPipelineAndEarliestOplogEntryAfterToken) {
    Timestamp resumeTimestamp(100, 1);
    Timestamp oplogTimestamp(100, 2);
    Timestamp docTimestamp(100, 3);

    auto dsCheckResumability = createDSCheckResumability(resumeTimestamp);
    addOplogEntryOnOtherNS(oplogTimestamp);
    addOplogEntryOnTestNS(docTimestamp, "ID");
    ASSERT_THROWS_CODE(
        dsCheckResumability->getNext(), AssertionException, ErrorCodes::ChangeStreamHistoryLost);
}

TEST_F(CheckResumabilityTest,
       ShouldFailWithoutReadingLaterDocumentsInPipelineIfEarliestOplogEntryAfterToken) {
    Timestamp resumeTimestamp(100, 1);
    Timestamp oplogTimestamp(100, 2);
    Timestamp docTimestamp(100, 3);

    auto dsCheckResumability = createDSCheckResumability(resumeTimestamp);
    addOplogEntryOnOtherNS(oplogTimestamp);
    addOplogEntryOnTestNS(docTimestamp, "ID");
    // Confirm that there are two documents queued in the mock oplog.
    ASSERT_EQ(_mock->size(), 2);
    ASSERT_THROWS_CODE(
        dsCheckResumability->getNext(), AssertionException, ErrorCodes::ChangeStreamHistoryLost);
    // Confirm that only the first document was read before the assertion was thrown.
    ASSERT_EQ(_mock->size(), 1);
}

TEST_F(CheckResumabilityTest, ShouldIgnoreOplogAfterFirstDoc) {
    Timestamp oplogTimestamp(100, 1);
    Timestamp resumeTimestamp(100, 2);
    Timestamp oplogFutureTimestamp(100, 3);
    Timestamp docTimestamp(100, 4);

    auto dsCheckResumability = createDSCheckResumability(resumeTimestamp);
    addOplogEntryOnOtherNS(oplogTimestamp);
    addOplogEntryOnTestNS(docTimestamp, "ID");
    auto result1 = dsCheckResumability->getNext();
    ASSERT_TRUE(result1.isAdvanced());
    auto& doc1 = result1.getDocument();
    ASSERT_EQ(docTimestamp, ResumeToken::parse(doc1["_id"].getDocument()).getData().clusterTime);

    addOplogEntryOnOtherNS(oplogFutureTimestamp);
    auto result2 = dsCheckResumability->getNext();
    ASSERT_TRUE(result2.isEOF());
}

TEST_F(CheckResumabilityTest, ShouldSucceedWhenOplogEntriesExistBeforeAndAfterResumeToken) {
    Timestamp oplogTimestamp(100, 1);
    Timestamp resumeTimestamp(100, 2);
    Timestamp oplogFutureTimestamp(100, 3);
    Timestamp docTimestamp(100, 4);

    auto dsCheckResumability = createDSCheckResumability(resumeTimestamp);
    addOplogEntryOnOtherNS(oplogTimestamp);
    addOplogEntryOnOtherNS(oplogFutureTimestamp);
    addOplogEntryOnTestNS(docTimestamp, "ID");

    auto result1 = dsCheckResumability->getNext();
    ASSERT_TRUE(result1.isAdvanced());
    auto& doc1 = result1.getDocument();
    ASSERT_EQ(docTimestamp, ResumeToken::parse(doc1["_id"].getDocument()).getData().clusterTime);
    auto result2 = dsCheckResumability->getNext();
    ASSERT_TRUE(result2.isEOF());
}

TEST_F(CheckResumabilityTest, ShouldIgnoreOplogAfterFirstEOF) {
    Timestamp oplogTimestamp(100, 1);
    Timestamp resumeTimestamp(100, 2);
    Timestamp oplogFutureTimestamp(100, 3);

    auto dsCheckResumability = createDSCheckResumability(resumeTimestamp);
    addOplogEntryOnOtherNS(oplogTimestamp);
    auto result1 = dsCheckResumability->getNext();
    ASSERT_TRUE(result1.isEOF());

    addOplogEntryOnOtherNS(oplogFutureTimestamp);
    auto result2 = dsCheckResumability->getNext();
    ASSERT_TRUE(result2.isEOF());
}

TEST_F(CheckResumabilityTest, ShouldSwallowAllEventsAtSameClusterTimeUpToResumeToken) {
    Timestamp resumeTimestamp(100, 2);

    // Set up the DSCSCheckResumability to check for an exact event ResumeToken.
    ResumeTokenData token(resumeTimestamp, 0, 0, testUuid(), Value(Document{{"_id"_sd, "3"_sd}}));
    auto dsCheckResumability = createDSCheckResumability(token);

    // Add 2 events at the same clusterTime as the resume token but whose docKey sort before it.
    addOplogEntryOnTestNS(resumeTimestamp, "1");
    addOplogEntryOnTestNS(resumeTimestamp, "2");
    // Add the resume token, plus one further event whose docKey sorts after the token.
    addOplogEntryOnTestNS(resumeTimestamp, "3");
    addOplogEntryOnTestNS(resumeTimestamp, "4");

    // The first event we see should be the resume token...
    auto result = dsCheckResumability->getNext();
    ASSERT_TRUE(result.isAdvanced());
    auto doc = result.getDocument();
    ASSERT_EQ(token, ResumeToken::parse(doc["_id"].getDocument()).getData());
    // ... then the post-token event, and then finally EOF.
    result = dsCheckResumability->getNext();
    ASSERT_TRUE(result.isAdvanced());
    auto postResumeTokenDoc =
        ResumeToken({resumeTimestamp, 0, 0, testUuid(), Value(Document{{"_id"_sd, "4"_sd}})})
            .toDocument();
    ASSERT_DOCUMENT_EQ(result.getDocument()["_id"].getDocument(), postResumeTokenDoc);
    ASSERT_TRUE(dsCheckResumability->getNext().isEOF());
}

TEST_F(CheckResumabilityTest, ShouldSwallowAllEventsAtSameClusterTimePriorToResumeToken) {
    Timestamp resumeTimestamp(100, 2);

    // Set up the DSCSCheckResumability to check for an exact event ResumeToken.
    ResumeTokenData token(resumeTimestamp, 0, 0, testUuid(), Value(Document{{"_id"_sd, "3"_sd}}));
    auto dsCheckResumability = createDSCheckResumability(token);

    // Add 2 events at the same clusterTime as the resume token but whose docKey sort before it.
    addOplogEntryOnTestNS(resumeTimestamp, "1");
    addOplogEntryOnTestNS(resumeTimestamp, "2");
    // Add one further event whose docKey sorts after the token.
    addOplogEntryOnTestNS(resumeTimestamp, "4");

    // The first event we see should be the post-token event, followed by EOF.
    auto result = dsCheckResumability->getNext();
    ASSERT_TRUE(result.isAdvanced());
    auto postResumeTokenDoc =
        ResumeToken({resumeTimestamp, 0, 0, testUuid(), Value(Document{{"_id"_sd, "4"_sd}})})
            .toDocument();
    ASSERT_DOCUMENT_EQ(result.getDocument()["_id"].getDocument(), postResumeTokenDoc);
    ASSERT_TRUE(dsCheckResumability->getNext().isEOF());
}

}  // namespace
}  // namespace mongo
