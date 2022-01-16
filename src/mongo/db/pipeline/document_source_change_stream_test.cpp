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
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/bson/bson_helper.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_mock.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/matcher/schema/expression_internal_schema_object_match.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/change_stream_rewrite_helpers.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_change_stream_add_post_image.h"
#include "mongo/db/pipeline/document_source_change_stream_add_pre_image.h"
#include "mongo/db/pipeline/document_source_change_stream_check_invalidate.h"
#include "mongo/db/pipeline/document_source_change_stream_check_resumability.h"
#include "mongo/db/pipeline/document_source_change_stream_ensure_resume_token_present.h"
#include "mongo/db/pipeline/document_source_change_stream_oplog_match.h"
#include "mongo/db/pipeline/document_source_change_stream_transform.h"
#include "mongo/db/pipeline/document_source_change_stream_unwind_transaction.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/s/resharding/resharding_change_event_o2_field_gen.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/db/transaction_history_iterator.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

using boost::intrusive_ptr;
using repl::OplogEntry;
using repl::OpTypeEnum;
using std::list;
using std::string;
using std::vector;

using D = Document;
using V = Value;

using DSChangeStream = DocumentSourceChangeStream;

static const Timestamp kDefaultTs(100, 1);
static const repl::OpTime kDefaultOpTime(kDefaultTs, 1);
static const NamespaceString nss("unittests.change_stream");
static const BSONObj kDefaultSpec = fromjson("{$changeStream: {}}");

class ChangeStreamStageTestNoSetup : public AggregationContextFixture {
public:
    ChangeStreamStageTestNoSetup() : ChangeStreamStageTestNoSetup(nss) {}
    explicit ChangeStreamStageTestNoSetup(NamespaceString nsString)
        : AggregationContextFixture(nsString) {}
};

struct MockMongoInterface final : public StubMongoProcessInterface {

    // Used by operations which need to obtain the oplog's UUID.
    static const UUID& oplogUuid() {
        static const UUID* oplog_uuid = new UUID(UUID::gen());
        return *oplog_uuid;
    }

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

    MockMongoInterface(std::vector<FieldPath> fields,
                       std::vector<repl::OplogEntry> transactionEntries = {},
                       std::vector<Document> documentsForLookup = {})
        : _fields(std::move(fields)),
          _transactionEntries(std::move(transactionEntries)),
          _documentsForLookup{std::move(documentsForLookup)} {}

    // For tests of transactions that involve multiple oplog entries.
    std::unique_ptr<TransactionHistoryIteratorBase> createTransactionHistoryIterator(
        repl::OpTime time) const {
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
    BSONObj getCollectionOptions(OperationContext* opCtx, const NamespaceString& nss) {
        return BSON("uuid" << oplogUuid());
    }

    boost::optional<Document> lookupSingleDocument(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        UUID collectionUUID,
        const Document& documentKey,
        boost::optional<BSONObj> readConcern) final {
        Matcher matcher(documentKey.toBson(), expCtx);
        auto it = std::find_if(_documentsForLookup.begin(),
                               _documentsForLookup.end(),
                               [&](const Document& lookedUpDoc) {
                                   return matcher.matches(lookedUpDoc.toBson(), nullptr);
                               });
        return (it != _documentsForLookup.end() ? *it : boost::optional<Document>{});
    }

    // For "insert" tests.
    std::pair<std::vector<FieldPath>, bool> collectDocumentKeyFieldsForHostedCollection(
        OperationContext*, const NamespaceString&, UUID) const final {
        return {_fields, false};
    }

    std::vector<FieldPath> _fields;

    // Stores oplog entries associated with a commit operation, including the oplog entries that a
    // real DocumentSourceChangeStream would not see, because they are marked with a "prepare" or
    // "partialTxn" flag. When the DocumentSourceChangeStream sees the commit for the transaction,
    // either an explicit "commitCommand" or an implicit commit represented by an "applyOps" that is
    // not marked with the "prepare" or "partialTxn" flag, it uses a TransactionHistoryIterator to
    // go back and look up these entries.
    //
    // These entries are stored in the order they would be returned by the
    // TransactionHistoryIterator, which is the _reverse_ of the order they appear in the oplog.
    std::vector<repl::OplogEntry> _transactionEntries;

    // These documents are used to feed the 'lookupSingleDocument' method.
    std::vector<Document> _documentsForLookup;
};

class ChangeStreamStageTest : public ChangeStreamStageTestNoSetup {
public:
    ChangeStreamStageTest() : ChangeStreamStageTest(nss) {
        // Initialize the UUID on the ExpressionContext, to allow tests with a resumeToken.
        getExpCtx()->uuid = testUuid();
    };

    explicit ChangeStreamStageTest(NamespaceString nsString)
        : ChangeStreamStageTestNoSetup(nsString) {
        repl::ReplicationCoordinator::set(getExpCtx()->opCtx->getServiceContext(),
                                          std::make_unique<repl::ReplicationCoordinatorMock>(
                                              getExpCtx()->opCtx->getServiceContext()));
    }

    void checkTransformation(const OplogEntry& entry,
                             const boost::optional<Document> expectedDoc,
                             std::vector<FieldPath> docKeyFields = {},
                             const BSONObj& spec = kDefaultSpec,
                             const boost::optional<Document> expectedInvalidate = {},
                             const std::vector<repl::OplogEntry> transactionEntries = {},
                             std::vector<Document> documentsForLookup = {}) {
        vector<intrusive_ptr<DocumentSource>> stages = makeStages(entry.getEntry().toBSON(), spec);
        auto lastStage = stages.back();

        getExpCtx()->mongoProcessInterface = std::make_unique<MockMongoInterface>(
            docKeyFields, transactionEntries, std::move(documentsForLookup));

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
     */
    vector<intrusive_ptr<DocumentSource>> makeStages(const BSONObj& entry, const BSONObj& spec) {
        list<intrusive_ptr<DocumentSource>> result =
            DSChangeStream::createFromBson(spec.firstElement(), getExpCtx());
        vector<intrusive_ptr<DocumentSource>> stages(std::begin(result), std::end(result));
        getExpCtx()->mongoProcessInterface =
            std::make_unique<MockMongoInterface>(std::vector<FieldPath>{});

        // This match stage is a DocumentSourceChangeStreamOplogMatch, which we explicitly disallow
        // from executing as a safety mechanism, since it needs to use the collection-default
        // collation, even if the rest of the pipeline is using some other collation. To avoid ever
        // executing that stage here, we'll up-convert it from the non-executable
        // DocumentSourceChangeStreamOplogMatch to a fully-executable DocumentSourceMatch. This is
        // safe because all of the unit tests will use the 'simple' collation.
        auto match = dynamic_cast<DocumentSourceMatch*>(stages[0].get());
        ASSERT(match);
        auto executableMatch = DocumentSourceMatch::create(match->getQuery(), getExpCtx());
        // Replace the original match with the executable one.
        stages[0] = executableMatch;

        // Check the oplog entry is transformed correctly.
        auto transform = stages[2].get();
        ASSERT(transform);
        ASSERT(dynamic_cast<DocumentSourceChangeStreamTransform*>(transform));

        // Create mock stage and insert at the front of the stages.
        auto mock = DocumentSourceMock::createForTest(D(entry), getExpCtx());
        stages.insert(stages.begin(), mock);

        // Remove the DSEnsureResumeTokenPresent stage since it will swallow the result.
        auto newEnd = std::remove_if(stages.begin(), stages.end(), [](auto& stage) {
            return dynamic_cast<DocumentSourceChangeStreamEnsureResumeTokenPresent*>(stage.get());
        });
        stages.erase(newEnd, stages.end());

        // Wire up the stages by setting the source stage.
        auto prevIt = stages.begin();
        for (auto stageIt = stages.begin() + 1; stageIt != stages.end(); stageIt++) {
            auto stage = (*stageIt).get();
            stage->setSource((*prevIt).get());
            prevIt = stageIt;
        }

        return stages;
    }

    vector<intrusive_ptr<DocumentSource>> makeStages(const OplogEntry& entry) {
        return makeStages(entry.getEntry().toBSON(), kDefaultSpec);
    }

    OplogEntry createCommand(const BSONObj& oField,
                             const boost::optional<UUID> uuid = boost::none,
                             const boost::optional<bool> fromMigrate = boost::none,
                             boost::optional<repl::OpTime> opTime = boost::none) {
        return makeOplogEntry(OpTypeEnum::kCommand,  // op type
                              nss.getCommandNS(),    // namespace
                              oField,                // o
                              uuid,                  // uuid
                              fromMigrate,           // fromMigrate
                              boost::none,           // o2
                              opTime);               // opTime
    }

    Document makeResumeToken(Timestamp ts,
                             ImplicitValue uuid = Value(),
                             ImplicitValue docKey = Value(),
                             ResumeTokenData::FromInvalidate fromInvalidate =
                                 ResumeTokenData::FromInvalidate::kNotFromInvalidate,
                             size_t txnOpIndex = 0) {
        ResumeTokenData tokenData;
        tokenData.clusterTime = ts;
        tokenData.documentKey = docKey;
        tokenData.fromInvalidate = fromInvalidate;
        tokenData.txnOpIndex = txnOpIndex;
        if (!uuid.missing())
            tokenData.uuid = uuid.getUuid();
        return ResumeToken(tokenData).toDocument();
    }

    /**
     * Helper for running an applyOps through the pipeline, and getting all of the results.
     */
    std::vector<Document> getApplyOpsResults(const Document& applyOpsDoc,
                                             const LogicalSessionFromClient& lsid) {
        BSONObj applyOpsObj = applyOpsDoc.toBson();

        // Create an oplog entry and then glue on an lsid and txnNumber
        auto baseOplogEntry = makeOplogEntry(OpTypeEnum::kCommand,
                                             nss.getCommandNS(),
                                             applyOpsObj,
                                             testUuid(),
                                             boost::none,  // fromMigrate
                                             BSONObj());
        BSONObjBuilder builder(baseOplogEntry.getEntry().toBSON());
        builder.append("lsid", lsid.toBSON());
        builder.append("txnNumber", 0LL);
        BSONObj oplogEntry = builder.done();

        // Create the stages and check that the documents produced matched those in the applyOps.
        vector<intrusive_ptr<DocumentSource>> stages = makeStages(oplogEntry, kDefaultSpec);
        auto transform = stages[3].get();
        invariant(dynamic_cast<DocumentSourceChangeStreamTransform*>(transform) != nullptr);

        std::vector<Document> res;
        auto next = transform->getNext();
        while (next.isAdvanced()) {
            res.push_back(next.releaseDocument());
            next = transform->getNext();
        }
        return res;
    }


    /**
     * This method is required to avoid a static initialization fiasco resulting from calling
     * UUID::gen() in file static scope.
     */
    static const UUID& testUuid() {
        static const UUID* uuid_gen = new UUID(UUID::gen());
        return *uuid_gen;
    }

    static LogicalSessionFromClient testLsid() {
        // Required to avoid static initialization fiasco.
        static const UUID* uuid = new UUID(UUID::gen());
        LogicalSessionFromClient lsid{};
        lsid.setId(*uuid);
        return lsid;
    }

    /**
     * Creates an OplogEntry with given parameters and preset defaults for this test suite.
     */
    static repl::OplogEntry makeOplogEntry(
        repl::OpTypeEnum opType,
        NamespaceString nss,
        BSONObj object,
        boost::optional<UUID> uuid = testUuid(),
        boost::optional<bool> fromMigrate = boost::none,
        boost::optional<BSONObj> object2 = boost::none,
        boost::optional<repl::OpTime> opTime = boost::none,
        OperationSessionInfo sessionInfo = {},
        boost::optional<repl::OpTime> prevOpTime = {},
        boost::optional<repl::OpTime> preImageOpTime = boost::none) {
        long long hash = 1LL;
        return {
            repl::DurableOplogEntry(opTime ? *opTime : kDefaultOpTime,  // optime
                                    hash,                               // hash
                                    opType,                             // opType
                                    nss,                                // namespace
                                    uuid,                               // uuid
                                    fromMigrate,                        // fromMigrate
                                    repl::OplogEntry::kOplogVersion,    // version
                                    object,                             // o
                                    object2,                            // o2
                                    sessionInfo,                        // sessionInfo
                                    boost::none,                        // upsert
                                    Date_t(),                           // wall clock time
                                    {},                                 // statement ids
                                    prevOpTime,  // optime of previous write within same transaction
                                    preImageOpTime,  // pre-image optime
                                    boost::none,     // post-image optime
                                    boost::none,     // ShardId of resharding recipient
                                    boost::none,     // _id
                                    boost::none)};   // needsRetryImage
    }

    /**
     * Helper function to do a $v:2 delta oplog test.
     */
    void runUpdateV2OplogTest(BSONObj diff, Document updateModificationEntry) {
        BSONObj o2 = BSON("_id" << 1);
        auto deltaOplog = makeOplogEntry(OpTypeEnum::kUpdate,                // op type
                                         nss,                                // namespace
                                         BSON("diff" << diff << "$v" << 2),  // o
                                         testUuid(),                         // uuid
                                         boost::none,                        // fromMigrate
                                         o2);                                // o2
        // Update fields
        Document expectedUpdateField{
            {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), o2)},
            {DSChangeStream::kOperationTypeField, DSChangeStream::kUpdateOpType},
            {DSChangeStream::kClusterTimeField, kDefaultTs},
            {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
            {DSChangeStream::kDocumentKeyField, D{{"_id", 1}}},
            {
                "updateDescription",
                updateModificationEntry,
            },
        };
        checkTransformation(deltaOplog, expectedUpdateField);
    }

    /**
     * Helper to create change stream pipeline for testing.
     */
    std::unique_ptr<Pipeline, PipelineDeleter> buildTestPipeline(
        const std::vector<BSONObj>& rawPipeline) {
        auto expCtx = getExpCtx();
        expCtx->ns = NamespaceString("a.collection");
        expCtx->inMongos = true;

        auto pipeline = Pipeline::parse(rawPipeline, expCtx);
        pipeline->optimizePipeline();

        return pipeline;
    }

    /**
     * Helper to verify if the change stream pipeline contains expected stages.
     */
    void assertStagesNameOrder(std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
                               const std::vector<std::string> expectedStages) {
        ASSERT_EQ(pipeline->getSources().size(), expectedStages.size());

        auto stagesItr = pipeline->getSources().begin();
        auto expectedStagesItr = expectedStages.begin();

        while (expectedStagesItr != expectedStages.end()) {
            ASSERT_EQ(*expectedStagesItr, stagesItr->get()->getSourceName());
            ++expectedStagesItr;
            ++stagesItr;
        }
    }
};

bool getCSRewriteFeatureFlagValue() {
    return feature_flags::gFeatureFlagChangeStreamsRewrite.isEnabledAndIgnoreFCV();
}

bool isChangeStreamPreAndPostImagesEnabled() {
    return feature_flags::gFeatureFlagChangeStreamPreAndPostImages.isEnabledAndIgnoreFCV();
}

TEST_F(ChangeStreamStageTest, ShouldRejectNonObjectArg) {
    auto expCtx = getExpCtx();

    ASSERT_THROWS_CODE(DSChangeStream::createFromBson(
                           BSON(DSChangeStream::kStageName << "invalid").firstElement(), expCtx),
                       AssertionException,
                       50808);

    ASSERT_THROWS_CODE(DSChangeStream::createFromBson(
                           BSON(DSChangeStream::kStageName << 12345).firstElement(), expCtx),
                       AssertionException,
                       50808);
}

TEST_F(ChangeStreamStageTest, ShouldRejectUnrecognizedOption) {
    auto expCtx = getExpCtx();

    ASSERT_THROWS_CODE(
        DSChangeStream::createFromBson(
            BSON(DSChangeStream::kStageName << BSON("unexpected" << 4)).firstElement(), expCtx),
        AssertionException,
        40415);

    // In older versions this option was accepted.
    ASSERT_THROWS_CODE(DSChangeStream::createFromBson(
                           BSON(DSChangeStream::kStageName << BSON(
                                    "$_resumeAfterClusterTime" << BSON("ts" << Timestamp(0, 1))))
                               .firstElement(),
                           expCtx),
                       AssertionException,
                       40415);
}

TEST_F(ChangeStreamStageTest, ShouldRejectNonStringFullDocumentOption) {
    auto expCtx = getExpCtx();

    ASSERT_THROWS_CODE(
        DSChangeStream::createFromBson(
            BSON(DSChangeStream::kStageName << BSON("fullDocument" << true)).firstElement(),
            expCtx),
        AssertionException,
        ErrorCodes::TypeMismatch);
}

TEST_F(ChangeStreamStageTest, ShouldRejectUnrecognizedFullDocumentOption) {
    auto expCtx = getExpCtx();

    ASSERT_THROWS_CODE(
        DSChangeStream::createFromBson(BSON(DSChangeStream::kStageName << BSON("fullDocument"
                                                                               << "unrecognized"))
                                           .firstElement(),
                                       expCtx),
        AssertionException,
        ErrorCodes::BadValue);
}

TEST_F(ChangeStreamStageTest, ShouldRejectUnsupportedFullDocumentOption) {
    auto expCtx = getExpCtx();

    // New modes that are supposed to be working only when pre-/post-images feature flag is on.
    FullDocumentModeEnum modes[] = {FullDocumentModeEnum::kWhenAvailable,
                                    FullDocumentModeEnum::kRequired};

    for (const auto& mode : modes) {
        auto spec =
            BSON("$changeStream: " << DocumentSourceChangeStreamAddPostImageSpec(mode).toBSON());

        // TODO SERVER-58584: remove the feature flag.
        {
            RAIIServerParameterControllerForTest controller(
                "featureFlagChangeStreamPreAndPostImages", false);
            ASSERT_FALSE(isChangeStreamPreAndPostImagesEnabled());

            // 'DSChangeStream' is not allowed to be instantiated with new document modes when
            // pre-/post-images feature flag is disabled.
            ASSERT_THROWS_CODE(DSChangeStream::createFromBson(spec.firstElement(), expCtx),
                               AssertionException,
                               ErrorCodes::BadValue);
        }
        {
            RAIIServerParameterControllerForTest controller(
                "featureFlagChangeStreamPreAndPostImages", true);
            ASSERT(isChangeStreamPreAndPostImagesEnabled());

            // 'DSChangeStream' is allowed to be instantiated with new document modes when
            // pre-/post-images feature flag is enabled.
            DSChangeStream::createFromBson(spec.firstElement(), expCtx);
        }
    }
}

TEST_F(ChangeStreamStageTest, ShouldRejectBothStartAtOperationTimeAndResumeAfterOptions) {
    auto expCtx = getExpCtx();

    // Need to put the collection in the collection catalog so the resume token is valid.
    std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);
    CollectionCatalog::write(expCtx->opCtx, [&](CollectionCatalog& catalog) {
        catalog.registerCollection(expCtx->opCtx, testUuid(), std::move(collection));
    });

    ASSERT_THROWS_CODE(
        DSChangeStream::createFromBson(
            BSON(DSChangeStream::kStageName
                 << BSON("resumeAfter"
                         << makeResumeToken(kDefaultTs, testUuid(), BSON("x" << 2 << "_id" << 1))
                         << "startAtOperationTime" << kDefaultTs))
                .firstElement(),
            expCtx),
        AssertionException,
        40674);
}

TEST_F(ChangeStreamStageTest, ShouldRejectBothStartAfterAndResumeAfterOptions) {
    auto expCtx = getExpCtx();
    auto opCtx = expCtx->opCtx;

    // Need to put the collection in the collection catalog so the resume token is validcollection
    std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);
    CollectionCatalog::write(opCtx, [&](CollectionCatalog& catalog) {
        catalog.registerCollection(opCtx, testUuid(), std::move(collection));
    });

    ASSERT_THROWS_CODE(
        DSChangeStream::createFromBson(
            BSON(DSChangeStream::kStageName
                 << BSON("resumeAfter"
                         << makeResumeToken(kDefaultTs, testUuid(), BSON("x" << 2 << "_id" << 1))
                         << "startAfter"
                         << makeResumeToken(kDefaultTs, testUuid(), BSON("x" << 2 << "_id" << 1))))
                .firstElement(),
            expCtx),
        AssertionException,
        50865);
}

TEST_F(ChangeStreamStageTest, ShouldRejectBothStartAtOperationTimeAndStartAfterOptions) {
    auto expCtx = getExpCtx();
    auto opCtx = expCtx->opCtx;

    // Need to put the collection in the collection catalog so the resume token is valid.
    std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);
    CollectionCatalog::write(opCtx, [&](CollectionCatalog& catalog) {
        catalog.registerCollection(opCtx, testUuid(), std::move(collection));
    });

    ASSERT_THROWS_CODE(
        DSChangeStream::createFromBson(
            BSON(DSChangeStream::kStageName
                 << BSON("startAfter"
                         << makeResumeToken(kDefaultTs, testUuid(), BSON("x" << 2 << "_id" << 1))
                         << "startAtOperationTime" << kDefaultTs))
                .firstElement(),
            expCtx),
        AssertionException,
        40674);
}

TEST_F(ChangeStreamStageTest, ShouldRejectResumeAfterWithResumeTokenMissingUUID) {
    auto expCtx = getExpCtx();
    auto opCtx = expCtx->opCtx;

    // Need to put the collection in the collection catalog so the resume token is valid.
    std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);
    CollectionCatalog::write(opCtx, [&](CollectionCatalog& catalog) {
        catalog.registerCollection(opCtx, testUuid(), std::move(collection));
    });

    ASSERT_THROWS_CODE(
        DSChangeStream::createFromBson(
            BSON(DSChangeStream::kStageName << BSON("resumeAfter" << makeResumeToken(kDefaultTs)))
                .firstElement(),
            expCtx),
        AssertionException,
        ErrorCodes::InvalidResumeToken);
}

TEST_F(ChangeStreamStageTestNoSetup, FailsWithNoReplicationCoordinator) {
    const auto spec = fromjson("{$changeStream: {}}");

    ASSERT_THROWS_CODE(DocumentSourceChangeStream::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       40573);
}

TEST_F(ChangeStreamStageTest, CannotCreateStageForSystemCollection) {
    auto expressionContext = getExpCtx();
    expressionContext->ns = NamespaceString{"db", "system.namespace"};
    const auto spec = fromjson("{$changeStream: {allowToRunOnSystemNS: false}}");
    ASSERT_THROWS_CODE(DocumentSourceChangeStream::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::InvalidNamespace);
}

TEST_F(ChangeStreamStageTest, CanCreateStageForSystemCollectionWhenAllowToRunOnSystemNSIsTrue) {
    auto expressionContext = getExpCtx();
    expressionContext->ns = NamespaceString{"db", "system.namespace"};
    expressionContext->inMongos = false;
    const auto spec = fromjson("{$changeStream: {allowToRunOnSystemNS: true}}");
    DocumentSourceChangeStream::createFromBson(spec.firstElement(), getExpCtx());
}

TEST_F(ChangeStreamStageTest,
       CannotCreateStageForSystemCollectionWhenAllowToRunOnSystemNSIsTrueAndInMongos) {
    auto expressionContext = getExpCtx();
    expressionContext->ns = NamespaceString{"db", "system.namespace"};
    expressionContext->inMongos = true;
    const auto spec = fromjson("{$changeStream: {allowToRunOnSystemNS: true}}");
    ASSERT_THROWS_CODE(DocumentSourceChangeStream::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::InvalidNamespace);
}

TEST_F(ChangeStreamStageTest, CanCreateStageForNonSystemCollection) {
    const auto spec = fromjson("{$changeStream: {}}");
    DocumentSourceChangeStream::createFromBson(spec.firstElement(), getExpCtx());
}

TEST_F(ChangeStreamStageTest, ShowMigrationsFailsOnMongos) {
    auto expCtx = getExpCtx();
    expCtx->inMongos = true;
    auto spec = fromjson("{$changeStream: {showMigrationEvents: true}}");

    ASSERT_THROWS_CODE(
        DSChangeStream::createFromBson(spec.firstElement(), expCtx), AssertionException, 31123);
}

TEST_F(ChangeStreamStageTest, TransformInsertDocKeyXAndId) {
    auto insert = makeOplogEntry(OpTypeEnum::kInsert,           // op type
                                 nss,                           // namespace
                                 BSON("_id" << 1 << "x" << 2),  // o
                                 testUuid(),                    // uuid
                                 boost::none,                   // fromMigrate
                                 boost::none);                  // o2

    Document expectedInsert{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), BSON("x" << 2 << "_id" << 1))},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kFullDocumentField, D{{"_id", 1}, {"x", 2}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"x", 2}, {"_id", 1}}},  // Note _id <-> x reversal.
    };
    checkTransformation(insert, expectedInsert, {{"x"}, {"_id"}});
    bool fromMigrate = false;  // also check actual "fromMigrate: false" not filtered
    auto insert2 = makeOplogEntry(insert.getOpType(),    // op type
                                  insert.getNss(),       // namespace
                                  insert.getObject(),    // o
                                  insert.getUuid(),      // uuid
                                  fromMigrate,           // fromMigrate
                                  insert.getObject2());  // o2
    checkTransformation(insert2, expectedInsert, {{"x"}, {"_id"}});
}

TEST_F(ChangeStreamStageTest, TransformInsertDocKeyIdAndX) {
    auto insert = makeOplogEntry(OpTypeEnum::kInsert,           // op type
                                 nss,                           // namespace
                                 BSON("x" << 2 << "_id" << 1),  // o
                                 testUuid(),                    // uuid
                                 boost::none,                   // fromMigrate
                                 boost::none);                  // o2

    Document expectedInsert{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), BSON("_id" << 1 << "x" << 2))},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kFullDocumentField, D{{"x", 2}, {"_id", 1}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},  // _id first
    };
    checkTransformation(insert, expectedInsert, {{"_id"}, {"x"}});
}

TEST_F(ChangeStreamStageTest, TransformInsertDocKeyJustId) {
    auto insert = makeOplogEntry(OpTypeEnum::kInsert,           // op type
                                 nss,                           // namespace
                                 BSON("_id" << 1 << "x" << 2),  // o
                                 testUuid(),                    // uuid
                                 boost::none,                   // fromMigrate
                                 boost::none);                  // o2

    Document expectedInsert{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), BSON("_id" << 1))},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kFullDocumentField, D{{"_id", 1}, {"x", 2}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}}},
    };
    checkTransformation(insert, expectedInsert, {{"_id"}});
}

TEST_F(ChangeStreamStageTest, TransformInsertFromMigrate) {
    bool fromMigrate = true;
    auto insert = makeOplogEntry(OpTypeEnum::kInsert,           // op type
                                 nss,                           // namespace
                                 BSON("_id" << 1 << "x" << 1),  // o
                                 boost::none,                   // uuid
                                 fromMigrate,                   // fromMigrate
                                 boost::none);                  // o2

    checkTransformation(insert, boost::none);
}

TEST_F(ChangeStreamStageTest, TransformInsertFromMigrateShowMigrations) {
    bool fromMigrate = true;
    auto insert = makeOplogEntry(OpTypeEnum::kInsert,           // op type
                                 nss,                           // namespace
                                 BSON("x" << 2 << "_id" << 1),  // o
                                 testUuid(),                    // uuid
                                 fromMigrate,                   // fromMigrate
                                 boost::none);                  // o2

    auto spec = fromjson("{$changeStream: {showMigrationEvents: true}}");
    Document expectedInsert{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), BSON("_id" << 1 << "x" << 2))},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kFullDocumentField, D{{"x", 2}, {"_id", 1}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},  // _id first
    };
    checkTransformation(insert, expectedInsert, {{"_id"}, {"x"}}, spec);
}

TEST_F(ChangeStreamStageTest, TransformUpdateFields) {
    BSONObj o = BSON("$set" << BSON("y" << 1));
    BSONObj o2 = BSON("_id" << 1 << "x" << 2);
    auto updateField = makeOplogEntry(OpTypeEnum::kUpdate,  // op type
                                      nss,                  // namespace
                                      o,                    // o
                                      testUuid(),           // uuid
                                      boost::none,          // fromMigrate
                                      o2);                  // o2

    // Update fields
    Document expectedUpdateField{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), o2)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kUpdateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},
        {
            "updateDescription",
            D{{"updatedFields", D{{"y", 1}}}, {"removedFields", vector<V>()}},
        },
    };
    checkTransformation(updateField, expectedUpdateField);
}

TEST_F(ChangeStreamStageTest, TransformSimpleDeltaOplogUpdatedFields) {
    BSONObj diff = BSON("u" << BSON("a" << 1 << "b"
                                        << "updated"));

    runUpdateV2OplogTest(diff,
                         D{{"updatedFields", D{{"a", 1}, {"b", "updated"_sd}}},
                           {"removedFields", vector<V>{}},
                           {"truncatedArrays", vector<V>{}}});
}

TEST_F(ChangeStreamStageTest, TransformSimpleDeltaOplogInsertFields) {
    BSONObj diff = BSON("i" << BSON("a" << 1 << "b"
                                        << "updated"));

    runUpdateV2OplogTest(diff,
                         D{{"updatedFields", D{{"a", 1}, {"b", "updated"_sd}}},
                           {"removedFields", vector<V>{}},
                           {"truncatedArrays", vector<V>{}}});
}

TEST_F(ChangeStreamStageTest, TransformSimpleDeltaOplogRemovedFields) {
    BSONObj diff = BSON("d" << BSON("a" << false << "b" << false));

    runUpdateV2OplogTest(diff,
                         D{{"updatedFields", D{}},
                           {"removedFields", vector<V>{V("a"_sd), V("b"_sd)}},
                           {"truncatedArrays", vector<V>{}}});
}

TEST_F(ChangeStreamStageTest, TransformComplexDeltaOplog) {
    BSONObj diff = fromjson(
        "{"
        "   d: { a: false, b: false },"
        "   u: { c: 1, d: \"updated\" },"
        "   i: { e: 2, f: 3 }"
        "}");

    runUpdateV2OplogTest(diff,
                         D{{"updatedFields", D{{"c", 1}, {"d", "updated"_sd}, {"e", 2}, {"f", 3}}},
                           {"removedFields", vector<V>{V("a"_sd), V("b"_sd)}},
                           {"truncatedArrays", vector<V>{}}});
}

TEST_F(ChangeStreamStageTest, TransformDeltaOplogSubObjectDiff) {
    BSONObj diff = fromjson(
        "{"
        "   u: { c: 1, d: \"updated\" },"
        "   ssubObj: {"
        "           d: { a: false, b: false },"
        "           u: { c: 1, d: \"updated\" }"
        "   }"
        "}");

    runUpdateV2OplogTest(
        diff,
        D{{"updatedFields",
           D{{"c", 1}, {"d", "updated"_sd}, {"subObj.c", 1}, {"subObj.d", "updated"_sd}}},
          {"removedFields", vector<V>{V("subObj.a"_sd), V("subObj.b"_sd)}},
          {"truncatedArrays", vector<V>{}}});
}

TEST_F(ChangeStreamStageTest, TransformDeltaOplogSubArrayDiff) {
    BSONObj diff = fromjson(
        "{"
        "   sarrField: {a: true, l: 10,"
        "           u0: 1,"
        "           u1: {a: 1}},"
        "   sarrField2: {a: true, l: 20}"
        "   }"
        "}");

    runUpdateV2OplogTest(diff,
                         D{{"updatedFields", D{{"arrField.0", 1}, {"arrField.1", D{{"a", 1}}}}},
                           {"removedFields", vector<V>{}},
                           {"truncatedArrays",
                            vector<V>{V{D{{"field", "arrField"_sd}, {"newSize", 10}}},
                                      V{D{{"field", "arrField2"_sd}, {"newSize", 20}}}}}});
}

TEST_F(ChangeStreamStageTest, TransformDeltaOplogSubArrayDiffWithEmptyStringField) {
    BSONObj diff = fromjson(
        "{"
        "   s: {a: true, l: 10,"
        "           u0: 1,"
        "           u1: {a: 1}}"
        "}");

    runUpdateV2OplogTest(
        diff,
        D{{"updatedFields", D{{".0", 1}, {".1", D{{"a", 1}}}}},
          {"removedFields", vector<V>{}},
          {"truncatedArrays", vector<V>{V{D{{"field", ""_sd}, {"newSize", 10}}}}}});
}

TEST_F(ChangeStreamStageTest, TransformDeltaOplogNestedComplexSubDiffs) {
    BSONObj diff = fromjson(
        "{"
        "   u: { a: 1, b: 2},"
        "   sarrField: {a: true, l: 10,"
        "           u0: 1,"
        "           u1: {a: 1},"
        "           s2: { u: {a: 1}},"  // "arrField.2.a" should be updated.
        "           u4: 1,"             // Test updating non-contiguous fields.
        "           u6: 2},"
        "   ssubObj: {"
        "           d: {b: false},"  // "subObj.b" should be removed.
        "           u: {a: 1}}"      // "subObj.a" should be updated.
        "}");

    runUpdateV2OplogTest(
        diff,
        D{{"updatedFields",
           D{
               {"a", 1},
               {"b", 2},
               {"arrField.0", 1},
               {"arrField.1", D{{"a", 1}}},
               {"arrField.2.a", 1},
               {"arrField.4", 1},
               {"arrField.6", 2},
               {"subObj.a", 1},
           }},
          {"removedFields", vector<V>{V("subObj.b"_sd)}},
          {"truncatedArrays", vector<V>{V{D{{"field", "arrField"_sd}, {"newSize", 10}}}}}});
}

// Legacy documents might not have an _id field; then the document key is the full (post-update)
// document.
TEST_F(ChangeStreamStageTest, TransformUpdateFieldsLegacyNoId) {
    BSONObj o = BSON("$set" << BSON("y" << 1));
    BSONObj o2 = BSON("x" << 1 << "y" << 1);
    auto updateField = makeOplogEntry(OpTypeEnum::kUpdate,  // op type
                                      nss,                  // namespace
                                      o,                    // o
                                      testUuid(),           // uuid
                                      boost::none,          // fromMigrate
                                      o2);                  // o2

    // Update fields
    Document expectedUpdateField{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), o2)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kUpdateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"x", 1}, {"y", 1}}},
        {
            "updateDescription",
            D{{"updatedFields", D{{"y", 1}}}, {"removedFields", vector<V>()}},
        },
    };
    checkTransformation(updateField, expectedUpdateField);
}

TEST_F(ChangeStreamStageTest, TransformRemoveFields) {
    BSONObj o = BSON("$unset" << BSON("y" << 1));
    BSONObj o2 = BSON("_id" << 1 << "x" << 2);
    auto removeField = makeOplogEntry(OpTypeEnum::kUpdate,  // op type
                                      nss,                  // namespace
                                      o,                    // o
                                      testUuid(),           // uuid
                                      boost::none,          // fromMigrate
                                      o2);                  // o2

    // Remove fields
    Document expectedRemoveField{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), o2)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kUpdateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, Document{{"_id", 1}, {"x", 2}}},
        {
            "updateDescription",
            D{{"updatedFields", D{}}, {"removedFields", {"y"_sd}}},
        }};
    checkTransformation(removeField, expectedRemoveField);
}  // namespace

TEST_F(ChangeStreamStageTest, TransformReplace) {
    BSONObj o = BSON("_id" << 1 << "x" << 2 << "y" << 1);
    BSONObj o2 = BSON("_id" << 1 << "x" << 2);
    auto replace = makeOplogEntry(OpTypeEnum::kUpdate,  // op type
                                  nss,                  // namespace
                                  o,                    // o
                                  testUuid(),           // uuid
                                  boost::none,          // fromMigrate
                                  o2);                  // o2

    // Replace
    Document expectedReplace{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), o2)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kReplaceOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kFullDocumentField, D{{"_id", 1}, {"x", 2}, {"y", 1}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},
    };
    checkTransformation(replace, expectedReplace);
}

TEST_F(ChangeStreamStageTest, TransformDelete) {
    BSONObj o = BSON("_id" << 1 << "x" << 2);
    auto deleteEntry = makeOplogEntry(OpTypeEnum::kDelete,  // op type
                                      nss,                  // namespace
                                      o,                    // o
                                      testUuid(),           // uuid
                                      boost::none,          // fromMigrate
                                      boost::none);         // o2

    // Delete
    Document expectedDelete{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), o)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kDeleteOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},
    };
    checkTransformation(deleteEntry, expectedDelete);

    bool fromMigrate = false;  // also check actual "fromMigrate: false" not filtered
    auto deleteEntry2 = makeOplogEntry(deleteEntry.getOpType(),    // op type
                                       deleteEntry.getNss(),       // namespace
                                       deleteEntry.getObject(),    // o
                                       deleteEntry.getUuid(),      // uuid
                                       fromMigrate,                // fromMigrate
                                       deleteEntry.getObject2());  // o2

    checkTransformation(deleteEntry2, expectedDelete);
}

TEST_F(ChangeStreamStageTest, TransformDeleteFromMigrate) {
    bool fromMigrate = true;
    auto deleteEntry = makeOplogEntry(OpTypeEnum::kDelete,  // op type
                                      nss,                  // namespace
                                      BSON("_id" << 1),     // o
                                      boost::none,          // uuid
                                      fromMigrate,          // fromMigrate
                                      boost::none);         // o2

    checkTransformation(deleteEntry, boost::none);
}

TEST_F(ChangeStreamStageTest, TransformDeleteFromMigrateShowMigrations) {
    bool fromMigrate = true;
    BSONObj o = BSON("_id" << 1);
    auto deleteEntry = makeOplogEntry(OpTypeEnum::kDelete,  // op type
                                      nss,                  // namespace
                                      o,                    // o
                                      testUuid(),           // uuid
                                      fromMigrate,          // fromMigrate
                                      boost::none);         // o2

    auto spec = fromjson("{$changeStream: {showMigrationEvents: true}}");
    Document expectedDelete{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), o)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kDeleteOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}}},
    };

    checkTransformation(deleteEntry, expectedDelete, {}, spec);
}

TEST_F(ChangeStreamStageTest, TransformDrop) {
    OplogEntry dropColl = createCommand(BSON("drop" << nss.coll()), testUuid());

    Document expectedDrop{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid())},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kDropCollectionOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
    };
    Document expectedInvalidate{
        {DSChangeStream::kIdField,
         makeResumeToken(
             kDefaultTs, testUuid(), Value(), ResumeTokenData::FromInvalidate::kFromInvalidate)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInvalidateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
    };

    checkTransformation(dropColl, expectedDrop, {}, kDefaultSpec, expectedInvalidate);
}

TEST_F(ChangeStreamStageTest, TransformRename) {
    NamespaceString otherColl("test.bar");
    OplogEntry rename =
        createCommand(BSON("renameCollection" << nss.ns() << "to" << otherColl.ns()), testUuid());

    Document expectedRename{
        {DSChangeStream::kRenameTargetNssField,
         D{{"db", otherColl.db()}, {"coll", otherColl.coll()}}},
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid())},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kRenameCollectionOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
    };
    Document expectedInvalidate{
        {DSChangeStream::kIdField,
         makeResumeToken(
             kDefaultTs, testUuid(), Value(), ResumeTokenData::FromInvalidate::kFromInvalidate)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInvalidateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
    };

    checkTransformation(rename, expectedRename, {}, kDefaultSpec, expectedInvalidate);
}

TEST_F(ChangeStreamStageTest, TransformInvalidateFromMigrate) {
    NamespaceString otherColl("test.bar");

    bool dropCollFromMigrate = true;
    OplogEntry dropColl =
        createCommand(BSON("drop" << nss.coll()), testUuid(), dropCollFromMigrate);
    bool dropDBFromMigrate = true;
    OplogEntry dropDB = createCommand(BSON("dropDatabase" << 1), boost::none, dropDBFromMigrate);
    bool renameFromMigrate = true;
    OplogEntry rename =
        createCommand(BSON("renameCollection" << nss.ns() << "to" << otherColl.ns()),
                      boost::none,
                      renameFromMigrate);

    for (auto& entry : {dropColl, dropDB, rename}) {
        checkTransformation(entry, boost::none);
    }
}

TEST_F(ChangeStreamStageTest, TransformRenameTarget) {
    NamespaceString otherColl("test.bar");
    OplogEntry rename =
        createCommand(BSON("renameCollection" << otherColl.ns() << "to" << nss.ns()), testUuid());

    Document expectedRename{
        {DSChangeStream::kRenameTargetNssField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid())},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kRenameCollectionOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", otherColl.db()}, {"coll", otherColl.coll()}}},
    };
    Document expectedInvalidate{
        {DSChangeStream::kIdField,
         makeResumeToken(
             kDefaultTs, testUuid(), Value(), ResumeTokenData::FromInvalidate::kFromInvalidate)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInvalidateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
    };

    checkTransformation(rename, expectedRename, {}, kDefaultSpec, expectedInvalidate);
}

TEST_F(ChangeStreamStageTest, MatchFiltersDropDatabaseCommand) {
    OplogEntry dropDB = createCommand(BSON("dropDatabase" << 1), boost::none, false);
    checkTransformation(dropDB, boost::none);
}

TEST_F(ChangeStreamStageTest, TransformNewShardDetected) {
    auto o2Field = D{{"type", "migrateChunkToNewShard"_sd}};
    auto newShardDetected = makeOplogEntry(OpTypeEnum::kNoop,
                                           nss,
                                           BSONObj(),
                                           testUuid(),
                                           boost::none,  // fromMigrate
                                           o2Field.toBson());

    Document expectedNewShardDetected{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), BSON("_id" << o2Field))},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kNewShardDetectedOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
    };

    getExpCtx()->needsMerge = true;

    checkTransformation(newShardDetected, expectedNewShardDetected);
}

TEST_F(ChangeStreamStageTest, TransformReshardBegin) {
    auto uuid = UUID::gen();
    auto reshardingUuid = UUID::gen();

    ReshardingChangeEventO2Field o2Field{reshardingUuid, ReshardingChangeEventEnum::kReshardBegin};
    auto reshardingBegin = makeOplogEntry(OpTypeEnum::kNoop,
                                          nss,
                                          BSONObj(),
                                          uuid,
                                          true,  // fromMigrate
                                          o2Field.toBSON());

    auto spec = fromjson("{$changeStream: {showMigrationEvents: true}}");

    Document expectedReshardingBegin{
        {DSChangeStream::kReshardingUuidField, reshardingUuid},
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, uuid, BSON("_id" << o2Field.toBSON()))},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kReshardBeginOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
    };
    checkTransformation(reshardingBegin, expectedReshardingBegin, {}, spec);
}

TEST_F(ChangeStreamStageTest, TransformReshardDoneCatchUp) {
    auto existingUuid = UUID::gen();
    auto reshardingUuid = UUID::gen();
    auto temporaryNs = constructTemporaryReshardingNss(nss.db(), existingUuid);

    ReshardingChangeEventO2Field o2Field{reshardingUuid,
                                         ReshardingChangeEventEnum::kReshardDoneCatchUp};
    auto reshardDoneCatchUp = makeOplogEntry(OpTypeEnum::kNoop,
                                             temporaryNs,
                                             BSONObj(),
                                             reshardingUuid,
                                             true,  // fromMigrate
                                             o2Field.toBSON());

    auto spec =
        fromjson("{$changeStream: {showMigrationEvents: true, allowToRunOnSystemNS: true}}");
    auto expCtx = getExpCtx();
    expCtx->ns = temporaryNs;

    Document expectedReshardingDoneCatchUp{
        {DSChangeStream::kReshardingUuidField, reshardingUuid},
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, reshardingUuid, BSON("_id" << o2Field.toBSON()))},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kReshardDoneCatchUpOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
    };

    checkTransformation(reshardDoneCatchUp, expectedReshardingDoneCatchUp, {}, spec);
}

TEST_F(ChangeStreamStageTest, TransformEmptyApplyOps) {
    Document applyOpsDoc{{"applyOps", Value{std::vector<Document>{}}}};

    LogicalSessionFromClient lsid = testLsid();
    vector<Document> results = getApplyOpsResults(applyOpsDoc, lsid);

    // Should not return anything.
    ASSERT_EQ(results.size(), 0u);
}

DEATH_TEST_F(ChangeStreamStageTest, ShouldCrashWithNoopInsideApplyOps, "Unexpected noop") {
    Document applyOpsDoc =
        Document{{"applyOps",
                  Value{std::vector<Document>{
                      Document{{"op", "n"_sd},
                               {"ns", nss.ns()},
                               {"ui", testUuid()},
                               {"o", Value{Document{{"_id", 123}, {"x", "hallo"_sd}}}}}}}}};
    LogicalSessionFromClient lsid = testLsid();
    getApplyOpsResults(applyOpsDoc, lsid);  // Should crash.
}

DEATH_TEST_F(ChangeStreamStageTest,
             ShouldCrashWithEntryWithoutOpFieldInsideApplyOps,
             "Unexpected format for entry") {
    Document applyOpsDoc =
        Document{{"applyOps",
                  Value{std::vector<Document>{
                      Document{{"ns", nss.ns()},
                               {"ui", testUuid()},
                               {"o", Value{Document{{"_id", 123}, {"x", "hallo"_sd}}}}}}}}};
    LogicalSessionFromClient lsid = testLsid();
    getApplyOpsResults(applyOpsDoc, lsid);  // Should crash.
}

DEATH_TEST_F(ChangeStreamStageTest,
             ShouldCrashWithEntryWithNonStringOpFieldInsideApplyOps,
             "Unexpected format for entry") {
    Document applyOpsDoc =
        Document{{"applyOps",
                  Value{std::vector<Document>{
                      Document{{"op", 2},
                               {"ns", nss.ns()},
                               {"ui", testUuid()},
                               {"o", Value{Document{{"_id", 123}, {"x", "hallo"_sd}}}}}}}}};
    LogicalSessionFromClient lsid = testLsid();
    getApplyOpsResults(applyOpsDoc, lsid);  // Should crash.
}

TEST_F(ChangeStreamStageTest, TransformNonTransactionApplyOps) {
    BSONObj applyOpsObj = Document{{"applyOps",
                                    Value{std::vector<Document>{Document{
                                        {"op", "i"_sd},
                                        {"ns", nss.ns()},
                                        {"ui", testUuid()},
                                        {"o", Value{Document{{"_id", 123}, {"x", "hallo"_sd}}}}}}}}}
                              .toBson();

    // Don't append lsid or txnNumber

    auto oplogEntry = makeOplogEntry(OpTypeEnum::kCommand,
                                     nss.getCommandNS(),
                                     applyOpsObj,
                                     testUuid(),
                                     boost::none,  // fromMigrate
                                     BSONObj());


    checkTransformation(oplogEntry, boost::none);
}

TEST_F(ChangeStreamStageTest, TransformApplyOpsWithEntriesOnDifferentNs) {
    // Doesn't use the checkTransformation() pattern that other tests use since we expect multiple
    // documents to be returned from one applyOps.

    auto otherUUID = UUID::gen();
    Document applyOpsDoc{
        {"applyOps",
         Value{std::vector<Document>{
             Document{{"op", "i"_sd},
                      {"ns", "someotherdb.collname"_sd},
                      {"ui", otherUUID},
                      {"o", Value{Document{{"_id", 123}, {"x", "hallo"_sd}}}}},
             Document{{"op", "u"_sd},
                      {"ns", "someotherdb.collname"_sd},
                      {"ui", otherUUID},
                      {"o", Value{Document{{"$set", Value{Document{{"x", "hallo 2"_sd}}}}}}},
                      {"o2", Value{Document{{"_id", 123}}}}},
         }}},
    };
    LogicalSessionFromClient lsid = testLsid();
    vector<Document> results = getApplyOpsResults(applyOpsDoc, lsid);

    // All documents should be skipped.
    ASSERT_EQ(results.size(), 0u);
}

TEST_F(ChangeStreamStageTest, PreparedTransactionApplyOpsEntriesAreIgnored) {
    Document applyOpsDoc =
        Document{{"applyOps",
                  Value{std::vector<Document>{
                      Document{{"op", "i"_sd},
                               {"ns", nss.ns()},
                               {"ui", testUuid()},
                               {"o", Value{Document{{"_id", 123}, {"x", "hallo"_sd}}}}}}}},
                 {"prepare", true}};
    LogicalSessionFromClient lsid = testLsid();
    vector<Document> results = getApplyOpsResults(applyOpsDoc, lsid);

    // applyOps entries that are part of a prepared transaction are ignored. These entries will be
    // fetched for changeStreams delivery as part of transaction commit.
    ASSERT_EQ(results.size(), 0u);
}

TEST_F(ChangeStreamStageTest, CommitCommandReturnsOperationsFromPreparedTransaction) {
    // Create an oplog entry representing a prepared transaction.
    Document preparedApplyOps{
        {"applyOps",
         Value{std::vector<Document>{
             D{{"op", "i"_sd}, {"ns", nss.ns()}, {"ui", testUuid()}, {"o", V{D{{"_id", 123}}}}},
         }}},
        {"prepare", true},
    };

    repl::OpTime applyOpsOpTime(Timestamp(99, 1), 1);
    auto preparedTransaction = makeOplogEntry(OpTypeEnum::kCommand,
                                              nss.getCommandNS(),
                                              preparedApplyOps.toBson(),
                                              testUuid(),
                                              boost::none,  // fromMigrate
                                              boost::none,  // o2 field
                                              applyOpsOpTime);

    // Create an oplog entry representing the commit for the prepared transaction. The commit has a
    // 'prevWriteOpTimeInTransaction' value that matches the 'preparedApplyOps' entry, which the
    // MockMongoInterface will pretend is in the oplog.
    OperationSessionInfo sessionInfo;
    sessionInfo.setTxnNumber(1);
    sessionInfo.setSessionId(makeLogicalSessionIdForTest());
    auto oplogEntry =
        repl::DurableOplogEntry(kDefaultOpTime,                   // optime
                                1LL,                              // hash
                                OpTypeEnum::kCommand,             // opType
                                nss.getCommandNS(),               // namespace
                                boost::none,                      // uuid
                                boost::none,                      // fromMigrate
                                repl::OplogEntry::kOplogVersion,  // version
                                BSON("commitTransaction" << 1),   // o
                                boost::none,                      // o2
                                sessionInfo,                      // sessionInfo
                                boost::none,                      // upsert
                                Date_t(),                         // wall clock time
                                {},                               // statement ids
                                applyOpsOpTime,  // optime of previous write within same transaction
                                boost::none,     // pre-image optime
                                boost::none,     // post-image optime
                                boost::none,     // ShardId of resharding recipient
                                boost::none,     // _id
                                boost::none);    // needsRetryImage

    // When the DocumentSourceChangeStreamTransform sees the "commitTransaction" oplog entry, we
    // expect it to return the insert op within our 'preparedApplyOps' oplog entry.
    Document expectedResult{
        {DSChangeStream::kTxnNumberField, static_cast<int>(*sessionInfo.getTxnNumber())},
        {DSChangeStream::kLsidField, Document{{sessionInfo.getSessionId()->toBSON()}}},
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), BSONObj())},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kFullDocumentField, D{{"_id", 123}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{}},
    };

    checkTransformation(oplogEntry, expectedResult, {}, kDefaultSpec, {}, {preparedTransaction});
}

TEST_F(ChangeStreamStageTest, TransactionWithMultipleOplogEntries) {
    OperationSessionInfo sessionInfo;
    sessionInfo.setTxnNumber(1);
    sessionInfo.setSessionId(makeLogicalSessionIdForTest());

    // Create two applyOps entries that together represent a whole transaction.
    repl::OpTime applyOpsOpTime1(Timestamp(100, 1), 1);
    Document applyOps1{
        {"applyOps",
         V{std::vector<Document>{
             D{{"op", "i"_sd},
               {"ns", nss.ns()},
               {"ui", testUuid()},
               {"o", V{Document{{"_id", 123}}}}},
             D{{"op", "i"_sd},
               {"ns", nss.ns()},
               {"ui", testUuid()},
               {"o", V{Document{{"_id", 456}}}}},
         }}},
        {"partialTxn", true},
    };

    auto transactionEntry1 = makeOplogEntry(OpTypeEnum::kCommand,
                                            nss.getCommandNS(),
                                            applyOps1.toBson(),
                                            testUuid(),
                                            boost::none,  // fromMigrate
                                            boost::none,  // o2 field
                                            applyOpsOpTime1,
                                            sessionInfo,
                                            repl::OpTime());

    repl::OpTime applyOpsOpTime2(Timestamp(100, 2), 1);
    Document applyOps2{
        {"applyOps",
         V{std::vector<Document>{
             D{{"op", "i"_sd}, {"ns", nss.ns()}, {"ui", testUuid()}, {"o", V{D{{"_id", 789}}}}},
         }}},
        /* The absence of the "partialTxn" and "prepare" fields indicates that this command commits
           the transaction. */
    };

    auto transactionEntry2 = makeOplogEntry(OpTypeEnum::kCommand,
                                            nss.getCommandNS(),
                                            applyOps2.toBson(),
                                            testUuid(),
                                            boost::none,  // fromMigrate
                                            boost::none,  // o2 field
                                            applyOpsOpTime2,
                                            sessionInfo,
                                            applyOpsOpTime1);

    // We do not use the checkTransformation() pattern that other tests use since we expect multiple
    // documents to be returned from one applyOps.
    auto stages = makeStages(transactionEntry2);
    auto transform = stages[3].get();
    invariant(dynamic_cast<DocumentSourceChangeStreamTransform*>(transform) != nullptr);

    // Populate the MockTransactionHistoryEditor in reverse chronological order.
    getExpCtx()->mongoProcessInterface = std::make_unique<MockMongoInterface>(
        std::vector<FieldPath>{},
        std::vector<repl::OplogEntry>{transactionEntry2, transactionEntry1});

    // We should get three documents from the change stream, based on the documents in the two
    // applyOps entries.
    auto next = transform->getNext();
    ASSERT(next.isAdvanced());
    auto nextDoc = next.releaseDocument();
    ASSERT_EQ(nextDoc[DSChangeStream::kTxnNumberField].getLong(), *sessionInfo.getTxnNumber());
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kInsertOpType);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["_id"].getInt(), 123);
    ASSERT_EQ(
        nextDoc["lsid"].getDocument().toBson().woCompare(sessionInfo.getSessionId()->toBSON()), 0);
    auto resumeToken = ResumeToken::parse(nextDoc["_id"].getDocument()).toDocument();
    ASSERT_DOCUMENT_EQ(resumeToken,
                       makeResumeToken(applyOpsOpTime2.getTimestamp(),
                                       testUuid(),
                                       V{D{}},
                                       ResumeTokenData::FromInvalidate::kNotFromInvalidate,
                                       0));

    next = transform->getNext();
    ASSERT(next.isAdvanced());
    nextDoc = next.releaseDocument();
    ASSERT_EQ(nextDoc[DSChangeStream::kTxnNumberField].getLong(), *sessionInfo.getTxnNumber());
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kInsertOpType);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["_id"].getInt(), 456);
    ASSERT_EQ(
        nextDoc["lsid"].getDocument().toBson().woCompare(sessionInfo.getSessionId()->toBSON()), 0);
    resumeToken = ResumeToken::parse(nextDoc["_id"].getDocument()).toDocument();
    ASSERT_DOCUMENT_EQ(resumeToken,
                       makeResumeToken(applyOpsOpTime2.getTimestamp(),
                                       testUuid(),
                                       V{D{}},
                                       ResumeTokenData::FromInvalidate::kNotFromInvalidate,
                                       1));

    next = transform->getNext();
    ASSERT(next.isAdvanced());
    nextDoc = next.releaseDocument();
    ASSERT_EQ(nextDoc[DSChangeStream::kTxnNumberField].getLong(), *sessionInfo.getTxnNumber());
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kInsertOpType);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["_id"].getInt(), 789);
    ASSERT_EQ(
        nextDoc["lsid"].getDocument().toBson().woCompare(sessionInfo.getSessionId()->toBSON()), 0);
    resumeToken = ResumeToken::parse(nextDoc["_id"].getDocument()).toDocument();
    ASSERT_DOCUMENT_EQ(resumeToken,
                       makeResumeToken(applyOpsOpTime2.getTimestamp(),
                                       testUuid(),
                                       V{D{}},
                                       ResumeTokenData::FromInvalidate::kNotFromInvalidate,
                                       2));
}

TEST_F(ChangeStreamStageTest, TransactionWithEmptyOplogEntries) {
    OperationSessionInfo sessionInfo;
    sessionInfo.setTxnNumber(1);
    sessionInfo.setSessionId(makeLogicalSessionIdForTest());

    // Create a transaction that is chained across 5 applyOps oplog entries. The first, third, and
    // final oplog entries in the transaction chain contain empty applyOps arrays. The test verifies
    // that change streams (1) correctly detect the transaction chain despite the fact that the
    // final applyOps, which implicitly commits the transaction, is empty; and (2) behaves correctly
    // upon encountering empty applyOps at other stages of the transaction chain.
    repl::OpTime applyOpsOpTime1(Timestamp(100, 1), 1);
    Document applyOps1{
        {"applyOps", V{std::vector<Document>{}}},
        {"partialTxn", true},
    };

    auto transactionEntry1 = makeOplogEntry(OpTypeEnum::kCommand,
                                            nss.getCommandNS(),
                                            applyOps1.toBson(),
                                            testUuid(),
                                            boost::none,  // fromMigrate
                                            boost::none,  // o2 field
                                            applyOpsOpTime1,
                                            sessionInfo,
                                            repl::OpTime());

    repl::OpTime applyOpsOpTime2(Timestamp(100, 2), 1);
    Document applyOps2{
        {"applyOps",
         V{std::vector<Document>{
             D{{"op", "i"_sd},
               {"ns", nss.ns()},
               {"ui", testUuid()},
               {"o", V{Document{{"_id", 123}}}}},
         }}},
        {"partialTxn", true},
    };

    auto transactionEntry2 = makeOplogEntry(OpTypeEnum::kCommand,
                                            nss.getCommandNS(),
                                            applyOps2.toBson(),
                                            testUuid(),
                                            boost::none,  // fromMigrate
                                            boost::none,  // o2 field
                                            applyOpsOpTime2,
                                            sessionInfo,
                                            applyOpsOpTime1);

    repl::OpTime applyOpsOpTime3(Timestamp(100, 3), 1);
    Document applyOps3{
        {"applyOps", V{std::vector<Document>{}}},
        {"partialTxn", true},
    };

    auto transactionEntry3 = makeOplogEntry(OpTypeEnum::kCommand,
                                            nss.getCommandNS(),
                                            applyOps3.toBson(),
                                            testUuid(),
                                            boost::none,  // fromMigrate
                                            boost::none,  // o2 field
                                            applyOpsOpTime3,
                                            sessionInfo,
                                            applyOpsOpTime2);

    repl::OpTime applyOpsOpTime4(Timestamp(100, 4), 1);
    Document applyOps4{
        {"applyOps",
         V{std::vector<Document>{D{{"op", "i"_sd},
                                   {"ns", nss.ns()},
                                   {"ui", testUuid()},
                                   {"o", V{Document{{"_id", 456}}}}}}}},
        {"partialTxn", true},
    };

    auto transactionEntry4 = makeOplogEntry(OpTypeEnum::kCommand,
                                            nss.getCommandNS(),
                                            applyOps4.toBson(),
                                            testUuid(),
                                            boost::none,  // fromMigrate
                                            boost::none,  // o2 field
                                            applyOpsOpTime4,
                                            sessionInfo,
                                            applyOpsOpTime3);

    repl::OpTime applyOpsOpTime5(Timestamp(100, 5), 1);
    Document applyOps5{
        {"applyOps", V{std::vector<Document>{}}},
        /* The absence of the "partialTxn" and "prepare" fields indicates that this command commits
           the transaction. */
    };

    auto transactionEntry5 = makeOplogEntry(OpTypeEnum::kCommand,
                                            nss.getCommandNS(),
                                            applyOps5.toBson(),
                                            testUuid(),
                                            boost::none,  // fromMigrate
                                            boost::none,  // o2 field
                                            applyOpsOpTime5,
                                            sessionInfo,
                                            applyOpsOpTime4);

    // We do not use the checkTransformation() pattern that other tests use since we expect multiple
    // documents to be returned from one applyOps.
    auto stages = makeStages(transactionEntry5);
    auto transform = stages[3].get();
    invariant(dynamic_cast<DocumentSourceChangeStreamTransform*>(transform) != nullptr);

    // Populate the MockTransactionHistoryEditor in reverse chronological order.
    getExpCtx()->mongoProcessInterface =
        std::make_unique<MockMongoInterface>(std::vector<FieldPath>{},
                                             std::vector<repl::OplogEntry>{transactionEntry5,
                                                                           transactionEntry4,
                                                                           transactionEntry3,
                                                                           transactionEntry2,
                                                                           transactionEntry1});

    // We should get three documents from the change stream, based on the documents in the two
    // applyOps entries.
    auto next = transform->getNext();
    ASSERT(next.isAdvanced());
    auto nextDoc = next.releaseDocument();
    ASSERT_EQ(nextDoc[DSChangeStream::kTxnNumberField].getLong(), *sessionInfo.getTxnNumber());
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kInsertOpType);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["_id"].getInt(), 123);
    ASSERT_EQ(
        nextDoc["lsid"].getDocument().toBson().woCompare(sessionInfo.getSessionId()->toBSON()), 0);
    auto resumeToken = ResumeToken::parse(nextDoc["_id"].getDocument()).toDocument();
    ASSERT_DOCUMENT_EQ(resumeToken,
                       makeResumeToken(applyOpsOpTime5.getTimestamp(),
                                       testUuid(),
                                       V{D{}},
                                       ResumeTokenData::FromInvalidate::kNotFromInvalidate,
                                       0));

    next = transform->getNext();
    ASSERT(next.isAdvanced());
    nextDoc = next.releaseDocument();
    ASSERT_EQ(nextDoc[DSChangeStream::kTxnNumberField].getLong(), *sessionInfo.getTxnNumber());
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kInsertOpType);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["_id"].getInt(), 456);
    ASSERT_EQ(
        nextDoc["lsid"].getDocument().toBson().woCompare(sessionInfo.getSessionId()->toBSON()), 0);
    resumeToken = ResumeToken::parse(nextDoc["_id"].getDocument()).toDocument();
    ASSERT_DOCUMENT_EQ(resumeToken,
                       makeResumeToken(applyOpsOpTime5.getTimestamp(),
                                       testUuid(),
                                       V{D{}},
                                       ResumeTokenData::FromInvalidate::kNotFromInvalidate,
                                       1));
}

TEST_F(ChangeStreamStageTest, TransactionWithOnlyEmptyOplogEntries) {
    OperationSessionInfo sessionInfo;
    sessionInfo.setTxnNumber(1);
    sessionInfo.setSessionId(makeLogicalSessionIdForTest());

    // Create a transaction that is chained across 2 applyOps oplog entries. This test verifies that
    // a change stream correctly reads an empty transaction and does not observe any events from it.
    repl::OpTime applyOpsOpTime1(Timestamp(100, 1), 1);
    Document applyOps1{
        {"applyOps", V{std::vector<Document>{}}},
        {"partialTxn", true},
    };

    auto transactionEntry1 = makeOplogEntry(OpTypeEnum::kCommand,
                                            nss.getCommandNS(),
                                            applyOps1.toBson(),
                                            testUuid(),
                                            boost::none,  // fromMigrate
                                            boost::none,  // o2 field
                                            applyOpsOpTime1,
                                            sessionInfo,
                                            repl::OpTime());

    repl::OpTime applyOpsOpTime2(Timestamp(100, 2), 1);
    Document applyOps2{
        {"applyOps", V{std::vector<Document>{}}},
        /* The absence of the "partialTxn" and "prepare" fields indicates that this command commits
           the transaction. */
    };

    auto transactionEntry2 = makeOplogEntry(OpTypeEnum::kCommand,
                                            nss.getCommandNS(),
                                            applyOps2.toBson(),
                                            testUuid(),
                                            boost::none,  // fromMigrate
                                            boost::none,  // o2 field
                                            applyOpsOpTime2,
                                            sessionInfo,
                                            applyOpsOpTime1);

    // We do not use the checkTransformation() pattern that other tests use since we expect multiple
    // documents to be returned from one applyOps.
    auto stages = makeStages(transactionEntry2);
    auto transform = stages[3].get();
    invariant(dynamic_cast<DocumentSourceChangeStreamTransform*>(transform) != nullptr);

    // Populate the MockTransactionHistoryEditor in reverse chronological order.
    getExpCtx()->mongoProcessInterface = std::make_unique<MockMongoInterface>(
        std::vector<FieldPath>{},
        std::vector<repl::OplogEntry>{transactionEntry2, transactionEntry1});

    // We should get three documents from the change stream, based on the documents in the two
    // applyOps entries.
    auto next = transform->getNext();
    ASSERT(!next.isAdvanced());
}

TEST_F(ChangeStreamStageTest, PreparedTransactionWithMultipleOplogEntries) {
    OperationSessionInfo sessionInfo;
    sessionInfo.setTxnNumber(1);
    sessionInfo.setSessionId(makeLogicalSessionIdForTest());

    // Create two applyOps entries that together represent a whole transaction.
    repl::OpTime applyOpsOpTime1(Timestamp(99, 1), 1);
    Document applyOps1{
        {"applyOps",
         V{std::vector<Document>{
             D{{"op", "i"_sd}, {"ns", nss.ns()}, {"ui", testUuid()}, {"o", V{D{{"_id", 123}}}}},
             D{{"op", "i"_sd}, {"ns", nss.ns()}, {"ui", testUuid()}, {"o", V{D{{"_id", 456}}}}},
         }}},
        {"partialTxn", true},
    };

    auto transactionEntry1 = makeOplogEntry(OpTypeEnum::kCommand,
                                            nss.getCommandNS(),
                                            applyOps1.toBson(),
                                            testUuid(),
                                            boost::none,  // fromMigrate
                                            boost::none,  // o2 field
                                            applyOpsOpTime1,
                                            sessionInfo,
                                            repl::OpTime());

    repl::OpTime applyOpsOpTime2(Timestamp(99, 2), 1);
    Document applyOps2{
        {"applyOps",
         V{std::vector<Document>{
             D{{"op", "i"_sd}, {"ns", nss.ns()}, {"ui", testUuid()}, {"o", V{D{{"_id", 789}}}}},
         }}},
        {"prepare", true},
    };

    auto transactionEntry2 = makeOplogEntry(OpTypeEnum::kCommand,
                                            nss.getCommandNS(),
                                            applyOps2.toBson(),
                                            testUuid(),
                                            boost::none,  // fromMigrate
                                            boost::none,  // o2 field
                                            applyOpsOpTime2,
                                            sessionInfo,
                                            applyOpsOpTime1);

    // Create an oplog entry representing the commit for the prepared transaction.
    auto commitEntry = repl::DurableOplogEntry(
        kDefaultOpTime,                   // optime
        1LL,                              // hash
        OpTypeEnum::kCommand,             // opType
        nss.getCommandNS(),               // namespace
        boost::none,                      // uuid
        boost::none,                      // fromMigrate
        repl::OplogEntry::kOplogVersion,  // version
        BSON("commitTransaction" << 1),   // o
        boost::none,                      // o2
        sessionInfo,                      // sessionInfo
        boost::none,                      // upsert
        Date_t(),                         // wall clock time
        {},                               // statement ids
        applyOpsOpTime2,                  // optime of previous write within same transaction
        boost::none,                      // pre-image optime
        boost::none,                      // post-image optime
        boost::none,                      // ShardId of resharding recipient
        boost::none,                      // _id
        boost::none);                     // needsRetryImage

    // We do not use the checkTransformation() pattern that other tests use since we expect multiple
    // documents to be returned from one applyOps.
    auto stages = makeStages(commitEntry);
    auto transform = stages[3].get();
    invariant(dynamic_cast<DocumentSourceChangeStreamTransform*>(transform) != nullptr);

    // Populate the MockTransactionHistoryEditor in reverse chronological order.
    getExpCtx()->mongoProcessInterface = std::make_unique<MockMongoInterface>(
        std::vector<FieldPath>{},
        std::vector<repl::OplogEntry>{commitEntry, transactionEntry2, transactionEntry1});

    // We should get three documents from the change stream, based on the documents in the two
    // applyOps entries.
    auto next = transform->getNext();
    ASSERT(next.isAdvanced());
    auto nextDoc = next.releaseDocument();
    ASSERT_EQ(nextDoc[DSChangeStream::kTxnNumberField].getLong(), *sessionInfo.getTxnNumber());
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kInsertOpType);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["_id"].getInt(), 123);
    ASSERT_EQ(
        nextDoc["lsid"].getDocument().toBson().woCompare(sessionInfo.getSessionId()->toBSON()), 0);
    auto resumeToken = ResumeToken::parse(nextDoc["_id"].getDocument()).toDocument();
    ASSERT_DOCUMENT_EQ(
        resumeToken,
        makeResumeToken(kDefaultOpTime.getTimestamp(),  // Timestamp of the commitCommand.
                        testUuid(),
                        V{D{}},
                        ResumeTokenData::FromInvalidate::kNotFromInvalidate,
                        0));

    next = transform->getNext();
    ASSERT(next.isAdvanced());
    nextDoc = next.releaseDocument();
    ASSERT_EQ(nextDoc[DSChangeStream::kTxnNumberField].getLong(), *sessionInfo.getTxnNumber());
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kInsertOpType);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["_id"].getInt(), 456);
    ASSERT_EQ(
        nextDoc["lsid"].getDocument().toBson().woCompare(sessionInfo.getSessionId()->toBSON()), 0);
    resumeToken = ResumeToken::parse(nextDoc["_id"].getDocument()).toDocument();
    ASSERT_DOCUMENT_EQ(
        resumeToken,
        makeResumeToken(kDefaultOpTime.getTimestamp(),  // Timestamp of the commitCommand.
                        testUuid(),
                        V{D{}},
                        ResumeTokenData::FromInvalidate::kNotFromInvalidate,
                        1));

    next = transform->getNext();
    ASSERT(next.isAdvanced());
    nextDoc = next.releaseDocument();
    ASSERT_EQ(nextDoc[DSChangeStream::kTxnNumberField].getLong(), *sessionInfo.getTxnNumber());
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kInsertOpType);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["_id"].getInt(), 789);
    ASSERT_EQ(
        nextDoc["lsid"].getDocument().toBson().woCompare(sessionInfo.getSessionId()->toBSON()), 0);
    resumeToken = ResumeToken::parse(nextDoc["_id"].getDocument()).toDocument();
    ASSERT_DOCUMENT_EQ(
        resumeToken,
        makeResumeToken(kDefaultOpTime.getTimestamp(),  // Timestamp of the commitCommand.
                        testUuid(),
                        V{D{}},
                        ResumeTokenData::FromInvalidate::kNotFromInvalidate,
                        2));

    next = transform->getNext();
    ASSERT(!next.isAdvanced());
}

TEST_F(ChangeStreamStageTest, PreparedTransactionEndingWithEmptyApplyOps) {
    OperationSessionInfo sessionInfo;
    sessionInfo.setTxnNumber(1);
    sessionInfo.setSessionId(makeLogicalSessionIdForTest());

    // Create two applyOps entries that together represent a whole transaction.
    repl::OpTime applyOpsOpTime1(Timestamp(99, 1), 1);
    Document applyOps1{
        {"applyOps",
         V{std::vector<Document>{
             D{{"op", "i"_sd}, {"ns", nss.ns()}, {"ui", testUuid()}, {"o", V{D{{"_id", 123}}}}},
             D{{"op", "i"_sd}, {"ns", nss.ns()}, {"ui", testUuid()}, {"o", V{D{{"_id", 456}}}}},
         }}},
        {"partialTxn", true},
    };

    auto transactionEntry1 = makeOplogEntry(OpTypeEnum::kCommand,
                                            nss.getCommandNS(),
                                            applyOps1.toBson(),
                                            testUuid(),
                                            boost::none,  // fromMigrate
                                            boost::none,  // o2 field
                                            applyOpsOpTime1,
                                            sessionInfo,
                                            repl::OpTime());

    repl::OpTime applyOpsOpTime2(Timestamp(99, 2), 1);
    Document applyOps2{
        {"applyOps", V{std::vector<Document>{}}},
        {"prepare", true},
    };

    // The second applyOps is empty.
    auto transactionEntry2 = makeOplogEntry(OpTypeEnum::kCommand,
                                            nss.getCommandNS(),
                                            applyOps2.toBson(),
                                            testUuid(),
                                            boost::none,  // fromMigrate
                                            boost::none,  // o2 field
                                            applyOpsOpTime2,
                                            sessionInfo,
                                            applyOpsOpTime1);

    // Create an oplog entry representing the commit for the prepared transaction.
    auto commitEntry = repl::DurableOplogEntry(
        kDefaultOpTime,                   // optime
        1LL,                              // hash
        OpTypeEnum::kCommand,             // opType
        nss.getCommandNS(),               // namespace
        boost::none,                      // uuid
        boost::none,                      // fromMigrate
        repl::OplogEntry::kOplogVersion,  // version
        BSON("commitTransaction" << 1),   // o
        boost::none,                      // o2
        sessionInfo,                      // sessionInfo
        boost::none,                      // upsert
        Date_t(),                         // wall clock time
        {},                               // statement ids
        applyOpsOpTime2,                  // optime of previous write within same transaction
        boost::none,                      // pre-image optime
        boost::none,                      // post-image optime
        boost::none,                      // ShardId of resharding recipient
        boost::none,                      // _id
        boost::none);                     // needsRetryImage

    // We do not use the checkTransformation() pattern that other tests use since we expect multiple
    // documents to be returned from one applyOps.
    auto stages = makeStages(commitEntry);
    auto transform = stages[3].get();
    invariant(dynamic_cast<DocumentSourceChangeStreamTransform*>(transform) != nullptr);

    // Populate the MockTransactionHistoryEditor in reverse chronological order.
    getExpCtx()->mongoProcessInterface = std::make_unique<MockMongoInterface>(
        std::vector<FieldPath>{},
        std::vector<repl::OplogEntry>{commitEntry, transactionEntry2, transactionEntry1});

    // We should get two documents from the change stream, based on the documents in the non-empty
    // applyOps entry.
    auto next = transform->getNext();
    ASSERT(next.isAdvanced());
    auto nextDoc = next.releaseDocument();
    ASSERT_EQ(nextDoc[DSChangeStream::kTxnNumberField].getLong(), *sessionInfo.getTxnNumber());
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kInsertOpType);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["_id"].getInt(), 123);
    ASSERT_EQ(
        nextDoc["lsid"].getDocument().toBson().woCompare(sessionInfo.getSessionId()->toBSON()), 0);
    auto resumeToken = ResumeToken::parse(nextDoc["_id"].getDocument()).toDocument();
    ASSERT_DOCUMENT_EQ(
        resumeToken,
        makeResumeToken(kDefaultOpTime.getTimestamp(),  // Timestamp of the commitCommand.
                        testUuid(),
                        V{D{}},
                        ResumeTokenData::FromInvalidate::kNotFromInvalidate,
                        0));

    next = transform->getNext();
    ASSERT(next.isAdvanced());
    nextDoc = next.releaseDocument();
    ASSERT_EQ(nextDoc[DSChangeStream::kTxnNumberField].getLong(), *sessionInfo.getTxnNumber());
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kInsertOpType);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["_id"].getInt(), 456);
    ASSERT_EQ(
        nextDoc["lsid"].getDocument().toBson().woCompare(sessionInfo.getSessionId()->toBSON()), 0);
    resumeToken = ResumeToken::parse(nextDoc["_id"].getDocument()).toDocument();
    ASSERT_DOCUMENT_EQ(
        resumeToken,
        makeResumeToken(kDefaultOpTime.getTimestamp(),  // Timestamp of the commitCommand.
                        testUuid(),
                        V{D{}},
                        ResumeTokenData::FromInvalidate::kNotFromInvalidate,
                        1));

    next = transform->getNext();
    ASSERT(!next.isAdvanced());
}

TEST_F(ChangeStreamStageTest, TransformApplyOps) {
    // Doesn't use the checkTransformation() pattern that other tests use since we expect multiple
    // documents to be returned from one applyOps.

    Document applyOpsDoc{
        {"applyOps",
         Value{std::vector<Document>{
             Document{{"op", "i"_sd},
                      {"ns", nss.ns()},
                      {"ui", testUuid()},
                      {"o", Value{Document{{"_id", 123}, {"x", "hallo"_sd}}}}},
             Document{{"op", "u"_sd},
                      {"ns", nss.ns()},
                      {"ui", testUuid()},
                      {"o", Value{Document{{"$set", Value{Document{{"x", "hallo 2"_sd}}}}}}},
                      {"o2", Value{Document{{"_id", 123}}}}},
             // Operation on another namespace which should be skipped.
             Document{{"op", "i"_sd},
                      {"ns", "someotherdb.collname"_sd},
                      {"ui", UUID::gen()},
                      {"o", Value{Document{{"_id", 0}, {"x", "Should not read this!"_sd}}}}},
         }}},
    };
    LogicalSessionFromClient lsid = testLsid();
    vector<Document> results = getApplyOpsResults(applyOpsDoc, lsid);

    // The third document should be skipped.
    ASSERT_EQ(results.size(), 2u);

    // Check that the first document is correct.
    auto nextDoc = results[0];
    ASSERT_EQ(nextDoc["txnNumber"].getLong(), 0LL);
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kInsertOpType);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["_id"].getInt(), 123);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["x"].getString(), "hallo");
    ASSERT_EQ(nextDoc["lsid"].getDocument().toBson().woCompare(lsid.toBSON()), 0);

    // Check the second document.
    nextDoc = results[1];
    ASSERT_EQ(nextDoc["txnNumber"].getLong(), 0LL);
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kUpdateOpType);
    ASSERT_EQ(nextDoc[DSChangeStream::kDocumentKeyField]["_id"].getInt(), 123);
    ASSERT_EQ(nextDoc[DSChangeStream::kUpdateDescriptionField]["updatedFields"]["x"].getString(),
              "hallo 2");
    ASSERT_EQ(nextDoc["lsid"].getDocument().toBson().woCompare(lsid.toBSON()), 0);

    // The third document is skipped.
}

TEST_F(ChangeStreamStageTest, ClusterTimeMatchesOplogEntry) {
    const Timestamp ts(3, 45);
    const long long term = 4;
    const auto opTime = repl::OpTime(ts, term);

    // Test the 'clusterTime' field is copied from the oplog entry for an update.
    BSONObj o = BSON("$set" << BSON("y" << 1));
    BSONObj o2 = BSON("_id" << 1 << "x" << 2);
    auto updateField = makeOplogEntry(OpTypeEnum::kUpdate,  // op type
                                      nss,                  // namespace
                                      o,                    // o
                                      testUuid(),           // uuid
                                      boost::none,          // fromMigrate
                                      o2,                   // o2
                                      opTime);              // opTime

    Document expectedUpdateField{
        {DSChangeStream::kIdField, makeResumeToken(ts, testUuid(), o2)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kUpdateOpType},
        {DSChangeStream::kClusterTimeField, ts},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},
        {
            "updateDescription",
            D{{"updatedFields", D{{"y", 1}}}, {"removedFields", vector<V>()}},
        },
    };
    checkTransformation(updateField, expectedUpdateField);

    // Test the 'clusterTime' field is copied from the oplog entry for a collection drop.
    OplogEntry dropColl =
        createCommand(BSON("drop" << nss.coll()), testUuid(), boost::none, opTime);

    Document expectedDrop{
        {DSChangeStream::kIdField, makeResumeToken(ts, testUuid())},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kDropCollectionOpType},
        {DSChangeStream::kClusterTimeField, ts},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
    };
    checkTransformation(dropColl, expectedDrop);

    // Test the 'clusterTime' field is copied from the oplog entry for a collection rename.
    NamespaceString otherColl("test.bar");
    OplogEntry rename =
        createCommand(BSON("renameCollection" << nss.ns() << "to" << otherColl.ns()),
                      testUuid(),
                      boost::none,
                      opTime);

    Document expectedRename{
        {DSChangeStream::kRenameTargetNssField,
         D{{"db", otherColl.db()}, {"coll", otherColl.coll()}}},
        {DSChangeStream::kIdField, makeResumeToken(ts, testUuid())},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kRenameCollectionOpType},
        {DSChangeStream::kClusterTimeField, ts},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
    };
    checkTransformation(rename, expectedRename);
}

TEST_F(ChangeStreamStageTest, MatchFiltersCreateCollection) {
    auto collSpec =
        D{{"create", "foo"_sd},
          {"idIndex", D{{"v", 2}, {"key", D{{"_id", 1}}}, {"name", "_id_"_sd}, {"ns", nss.ns()}}}};
    OplogEntry createColl = createCommand(collSpec.toBson(), testUuid());
    checkTransformation(createColl, boost::none);
}

TEST_F(ChangeStreamStageTest, MatchFiltersNoOp) {
    auto noOp = makeOplogEntry(OpTypeEnum::kNoop,  // op type
                               {},                 // namespace
                               BSON(repl::ReplicationCoordinator::newPrimaryMsgField
                                    << repl::ReplicationCoordinator::newPrimaryMsg));  // o

    checkTransformation(noOp, boost::none);
}

TEST_F(ChangeStreamStageTest, TransformationShouldBeAbleToReParseSerializedStage) {
    auto expCtx = getExpCtx();

    DocumentSourceChangeStreamSpec spec;
    spec.setStartAtOperationTime(kDefaultTs);
    auto originalSpec = BSON("" << spec.toBSON());

    auto result = DSChangeStream::createFromBson(originalSpec.firstElement(), expCtx);

    vector<intrusive_ptr<DocumentSource>> allStages(std::begin(result), std::end(result));

    ASSERT_EQ(allStages.size(), 5);

    auto stage = allStages[2];
    ASSERT(dynamic_cast<DocumentSourceChangeStreamTransform*>(stage.get()));

    //
    // Serialize the stage and confirm contents.
    //
    vector<Value> serialization;
    stage->serializeToArray(serialization);
    ASSERT_EQ(serialization.size(), 1UL);
    ASSERT_EQ(serialization[0].getType(), BSONType::Object);
    auto serializedDoc = serialization[0].getDocument();
    ASSERT_BSONOBJ_EQ(
        serializedDoc[DocumentSourceChangeStreamTransform::kStageName].getDocument().toBson(),
        originalSpec[""].Obj());

    //
    // Create a new stage from the serialization. Serialize the new stage and confirm that it is
    // equivalent to the original serialization.
    //
    auto serializedBson = serializedDoc.toBson();
    auto roundTripped = Pipeline::create(
        DSChangeStream::createFromBson(serializedBson.firstElement(), expCtx), expCtx);
    auto newSerialization = roundTripped->serialize();

    ASSERT_EQ(newSerialization.size(), 5UL);

    // DSCSTransform stage should be the third stage after DSCSOplogMatch and
    // DSCSUnwindTransactions stages.
    ASSERT_VALUE_EQ(newSerialization[2], serialization[0]);
}

TEST_F(ChangeStreamStageTest, DSCSTransformStageEmptySpecSerializeResumeAfter) {
    auto expCtx = getExpCtx();
    auto originalSpec = BSON(DSChangeStream::kStageName << BSONObj());

    // Verify that the 'initialPostBatchResumeToken' is populated while parsing.
    ASSERT(expCtx->initialPostBatchResumeToken.isEmpty());
    ON_BLOCK_EXIT([&expCtx] {
        // Reset for the next run.
        expCtx->initialPostBatchResumeToken = BSONObj();
    });

    auto result = DSChangeStream::createFromBson(originalSpec.firstElement(), expCtx);
    ASSERT(!expCtx->initialPostBatchResumeToken.isEmpty());

    vector<intrusive_ptr<DocumentSource>> allStages(std::begin(result), std::end(result));
    ASSERT_EQ(allStages.size(), 5);
    auto transformStage = allStages[2];
    ASSERT(dynamic_cast<DocumentSourceChangeStreamTransform*>(transformStage.get()));


    // Verify that an additional start point field is populated while serializing.
    vector<Value> serialization;
    transformStage->serializeToArray(serialization);
    ASSERT_EQ(serialization.size(), 1UL);
    ASSERT_EQ(serialization[0].getType(), BSONType::Object);
    ASSERT(!serialization[0]
                .getDocument()[DocumentSourceChangeStreamTransform::kStageName]
                .getDocument()[DocumentSourceChangeStreamSpec::kStartAtOperationTimeFieldName]
                .missing());
}

TEST_F(ChangeStreamStageTest, DSCSTransformStageWithResumeTokenSerialize) {
    auto expCtx = getExpCtx();

    DocumentSourceChangeStreamSpec spec;
    spec.setResumeAfter(ResumeToken::parse(makeResumeToken(kDefaultTs, testUuid())));
    auto originalSpec = BSON("" << spec.toBSON());

    // Verify that the 'initialPostBatchResumeToken' is populated while parsing.
    ASSERT(expCtx->initialPostBatchResumeToken.isEmpty());
    ON_BLOCK_EXIT([&expCtx] {
        // Reset for the next run.
        expCtx->initialPostBatchResumeToken = BSONObj();
    });

    auto stage =
        DocumentSourceChangeStreamTransform::createFromBson(originalSpec.firstElement(), expCtx);
    ASSERT(!expCtx->initialPostBatchResumeToken.isEmpty());

    vector<Value> serialization;
    stage->serializeToArray(serialization);
    ASSERT_EQ(serialization.size(), 1UL);
    ASSERT_EQ(serialization[0].getType(), BSONType::Object);
    ASSERT_BSONOBJ_EQ(serialization[0]
                          .getDocument()[DocumentSourceChangeStreamTransform::kStageName]
                          .getDocument()
                          .toBson(),
                      originalSpec[""].Obj());
}

template <typename Stage, typename StageSpec>
void validateDocumentSourceStageSerialization(
    StageSpec spec, BSONObj specAsBSON, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto stage = Stage::createFromBson(specAsBSON.firstElement(), expCtx);
    vector<Value> serialization;
    stage->serializeToArray(serialization);

    ASSERT_EQ(serialization.size(), 1UL);
    ASSERT_EQ(serialization[0].getType(), BSONType::Object);
    ASSERT_BSONOBJ_EQ(serialization[0].getDocument().toBson(),
                      BSON(Stage::kStageName << spec.toBSON()));
}

TEST_F(ChangeStreamStageTest, DSCSOplogMatchStageSerialization) {
    auto expCtx = getExpCtx();

    DocumentSourceChangeStreamOplogMatchSpec spec;
    auto dummyFilter = BSON("a" << 1);
    spec.setFilter(dummyFilter);
    auto stageSpecAsBSON = BSON("" << spec.toBSON());

    validateDocumentSourceStageSerialization<DocumentSourceChangeStreamOplogMatch>(
        std::move(spec), stageSpecAsBSON, expCtx);
}

TEST_F(ChangeStreamStageTest, DSCSUnwindTransactionStageSerialization) {
    auto expCtx = getExpCtx();

    auto filter = BSON("ns" << BSON("$regex"
                                    << "^db\\.coll$"));
    DocumentSourceChangeStreamUnwindTransactionSpec spec(filter);
    auto stageSpecAsBSON = BSON("" << spec.toBSON());

    validateDocumentSourceStageSerialization<DocumentSourceChangeStreamUnwindTransaction>(
        std::move(spec), stageSpecAsBSON, expCtx);
}

TEST_F(ChangeStreamStageTest, DSCSCheckInvalidateStageSerialization) {
    auto expCtx = getExpCtx();

    DocumentSourceChangeStreamCheckInvalidateSpec spec;
    spec.setStartAfterInvalidate(ResumeToken::parse(makeResumeToken(
        kDefaultTs, testUuid(), Value(), ResumeTokenData::FromInvalidate::kFromInvalidate)));
    auto stageSpecAsBSON = BSON("" << spec.toBSON());

    validateDocumentSourceStageSerialization<DocumentSourceChangeStreamCheckInvalidate>(
        std::move(spec), stageSpecAsBSON, expCtx);
}

TEST_F(ChangeStreamStageTest, DSCSResumabilityStageSerialization) {
    auto expCtx = getExpCtx();

    DocumentSourceChangeStreamCheckResumabilitySpec spec;
    spec.setResumeToken(ResumeToken::parse(makeResumeToken(kDefaultTs, testUuid())));
    auto stageSpecAsBSON = BSON("" << spec.toBSON());

    validateDocumentSourceStageSerialization<DocumentSourceChangeStreamCheckResumability>(
        std::move(spec), stageSpecAsBSON, expCtx);
}

TEST_F(ChangeStreamStageTest, DSCSLookupChangePreImageStageSerialization) {
    auto expCtx = getExpCtx();

    DocumentSourceChangeStreamAddPreImageSpec spec(FullDocumentBeforeChangeModeEnum::kRequired);
    auto stageSpecAsBSON = BSON("" << spec.toBSON());

    validateDocumentSourceStageSerialization<DocumentSourceChangeStreamAddPreImage>(
        std::move(spec), stageSpecAsBSON, expCtx);
}

TEST_F(ChangeStreamStageTest, DSCSLookupChangePostImageStageSerialization) {
    auto expCtx = getExpCtx();

    DocumentSourceChangeStreamAddPostImageSpec spec(FullDocumentModeEnum::kUpdateLookup);
    auto stageSpecAsBSON = BSON("" << spec.toBSON());

    validateDocumentSourceStageSerialization<DocumentSourceChangeStreamAddPostImage>(
        std::move(spec), stageSpecAsBSON, expCtx);
}

TEST_F(ChangeStreamStageTest, CloseCursorOnInvalidateEntries) {
    OplogEntry dropColl = createCommand(BSON("drop" << nss.coll()), testUuid());
    auto stages = makeStages(dropColl);
    auto lastStage = stages.back();

    Document expectedDrop{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid())},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kDropCollectionOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
    };
    Document expectedInvalidate{
        {DSChangeStream::kIdField,
         makeResumeToken(
             kDefaultTs, testUuid(), Value(), ResumeTokenData::FromInvalidate::kFromInvalidate)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInvalidateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
    };

    auto next = lastStage->getNext();
    // Transform into drop entry.
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), expectedDrop);
    next = lastStage->getNext();
    // Transform into invalidate entry.
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), expectedInvalidate);

    // Then throw an exception on the next call of getNext().
    ASSERT_THROWS(lastStage->getNext(), ExceptionFor<ErrorCodes::ChangeStreamInvalidated>);
}

TEST_F(ChangeStreamStageTest, CloseCursorEvenIfInvalidateEntriesGetFilteredOut) {
    OplogEntry dropColl = createCommand(BSON("drop" << nss.coll()), testUuid());
    auto stages = makeStages(dropColl);
    auto lastStage = stages.back();
    // Add a match stage after change stream to filter out the invalidate entries.
    auto match = DocumentSourceMatch::create(fromjson("{operationType: 'insert'}"), getExpCtx());
    match->setSource(lastStage.get());

    // Throw an exception on the call of getNext().
    ASSERT_THROWS(match->getNext(), ExceptionFor<ErrorCodes::ChangeStreamInvalidated>);
}

TEST_F(ChangeStreamStageTest, DocumentKeyShouldIncludeShardKeyFromResumeToken) {
    const Timestamp ts(3, 45);
    const long long term = 4;
    const auto opTime = repl::OpTime(ts, term);
    const auto uuid = testUuid();

    std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);
    CollectionCatalog::write(getExpCtx()->opCtx, [&](CollectionCatalog& catalog) {
        catalog.registerCollection(getExpCtx()->opCtx, uuid, std::move(collection));
    });


    BSONObj o2 = BSON("_id" << 1 << "shardKey" << 2);
    auto resumeToken = makeResumeToken(ts, uuid, o2);

    BSONObj insertDoc = BSON("_id" << 2 << "shardKey" << 3);
    auto insertEntry = makeOplogEntry(OpTypeEnum::kInsert,  // op type
                                      nss,                  // namespace
                                      insertDoc,            // o
                                      uuid,                 // uuid
                                      boost::none,          // fromMigrate
                                      boost::none,          // o2
                                      opTime);              // opTime

    Document expectedInsert{
        {DSChangeStream::kIdField, makeResumeToken(ts, uuid, insertDoc)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, ts},
        {DSChangeStream::kFullDocumentField, D{{"_id", 2}, {"shardKey", 3}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 2}, {"shardKey", 3}}},
    };
    // Although the chunk manager and sharding catalog are not aware of the shard key in this test,
    // the expectation is for the $changeStream stage to infer the shard key from the resume token.
    checkTransformation(
        insertEntry,
        expectedInsert,
        {{"_id"}},  // Mock the 'collectDocumentKeyFieldsForHostedCollection' response.
        BSON("$changeStream" << BSON("resumeAfter" << resumeToken)));

    // Verify the same behavior with resuming using 'startAfter'.
    checkTransformation(
        insertEntry,
        expectedInsert,
        {{"_id"}},  // Mock the 'collectDocumentKeyFieldsForHostedCollection' response.
        BSON("$changeStream" << BSON("startAfter" << resumeToken)));
}

TEST_F(ChangeStreamStageTest, DocumentKeyShouldNotIncludeShardKeyFieldsIfNotPresentInOplogEntry) {
    const Timestamp ts(3, 45);
    const long long term = 4;
    const auto opTime = repl::OpTime(ts, term);
    const auto uuid = testUuid();

    std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);
    CollectionCatalog::write(getExpCtx()->opCtx, [&](CollectionCatalog& catalog) {
        catalog.registerCollection(getExpCtx()->opCtx, uuid, std::move(collection));
    });

    BSONObj o2 = BSON("_id" << 1 << "shardKey" << 2);
    auto resumeToken = makeResumeToken(ts, uuid, o2);

    // Note that the 'o' field in the oplog entry does not contain the shard key field.
    BSONObj insertDoc = BSON("_id" << 2);
    auto insertEntry = makeOplogEntry(OpTypeEnum::kInsert,  // op type
                                      nss,                  // namespace
                                      insertDoc,            // o
                                      uuid,                 // uuid
                                      boost::none,          // fromMigrate
                                      boost::none,          // o2
                                      opTime);              // opTime

    Document expectedInsert{
        {DSChangeStream::kIdField, makeResumeToken(ts, uuid, insertDoc)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, ts},
        {DSChangeStream::kFullDocumentField, D{{"_id", 2}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 2}}},
    };
    checkTransformation(
        insertEntry,
        expectedInsert,
        {{"_id"}},  // Mock the 'collectDocumentKeyFieldsForHostedCollection' response.
        BSON("$changeStream" << BSON("resumeAfter" << resumeToken)));

    // Verify the same behavior with resuming using 'startAfter'.
    checkTransformation(
        insertEntry,
        expectedInsert,
        {{"_id"}},  // Mock the 'collectDocumentKeyFieldsForHostedCollection' response.
        BSON("$changeStream" << BSON("startAfter" << resumeToken)));
}

TEST_F(ChangeStreamStageTest, ResumeAfterFailsIfResumeTokenDoesNotContainUUID) {
    const Timestamp ts(3, 45);
    const auto uuid = testUuid();

    std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);
    CollectionCatalog::write(getExpCtx()->opCtx, [&](CollectionCatalog& catalog) {
        catalog.registerCollection(getExpCtx()->opCtx, uuid, std::move(collection));
    });

    // Create a resume token from only the timestamp.
    auto resumeToken = makeResumeToken(ts);

    ASSERT_THROWS_CODE(
        DSChangeStream::createFromBson(
            BSON(DSChangeStream::kStageName << BSON("resumeAfter" << resumeToken)).firstElement(),
            getExpCtx()),
        AssertionException,
        ErrorCodes::InvalidResumeToken);
}

TEST_F(ChangeStreamStageTest, RenameFromSystemToUserCollectionShouldIncludeNotification) {
    // Renaming to a non-system collection will include a notification in the stream.
    NamespaceString systemColl(nss.db() + ".system.users");
    OplogEntry rename =
        createCommand(BSON("renameCollection" << systemColl.ns() << "to" << nss.ns()), testUuid());

    // Note that the collection rename does *not* have the queued invalidated field.
    Document expectedRename{
        {DSChangeStream::kRenameTargetNssField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid())},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kRenameCollectionOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", systemColl.db()}, {"coll", systemColl.coll()}}},
    };
    checkTransformation(rename, expectedRename);
}

TEST_F(ChangeStreamStageTest, RenameFromUserToSystemCollectionShouldIncludeNotification) {
    // Renaming to a system collection will include a notification in the stream.
    NamespaceString systemColl(nss.db() + ".system.users");
    OplogEntry rename =
        createCommand(BSON("renameCollection" << nss.ns() << "to" << systemColl.ns()), testUuid());

    // Note that the collection rename does *not* have the queued invalidated field.
    Document expectedRename{
        {DSChangeStream::kRenameTargetNssField,
         D{{"db", systemColl.db()}, {"coll", systemColl.coll()}}},
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid())},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kRenameCollectionOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
    };
    checkTransformation(rename, expectedRename);
}

TEST_F(ChangeStreamStageTest, ResumeAfterWithTokenFromInvalidateShouldFail) {
    auto expCtx = getExpCtx();

    // Need to put the collection in the collection catalog so the resume token is valid.
    std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);
    CollectionCatalog::write(expCtx->opCtx, [&](CollectionCatalog& catalog) {
        catalog.registerCollection(getExpCtx()->opCtx, testUuid(), std::move(collection));
    });

    const auto resumeTokenInvalidate =
        makeResumeToken(kDefaultTs,
                        testUuid(),
                        BSON("x" << 2 << "_id" << 1),
                        ResumeTokenData::FromInvalidate::kFromInvalidate);

    ASSERT_THROWS_CODE(
        DSChangeStream::createFromBson(
            BSON(DSChangeStream::kStageName << BSON("resumeAfter" << resumeTokenInvalidate))
                .firstElement(),
            expCtx),
        AssertionException,
        ErrorCodes::InvalidResumeToken);
}

TEST_F(ChangeStreamStageTest, UsesResumeTokenAsSortKeyIfNeedsMergeIsFalse) {
    auto insert = makeOplogEntry(OpTypeEnum::kInsert,           // op type
                                 nss,                           // namespace
                                 BSON("x" << 2 << "_id" << 1),  // o
                                 testUuid(),                    // uuid
                                 boost::none,                   // fromMigrate
                                 boost::none);                  // o2

    auto stages = makeStages(insert.getEntry().toBSON(), kDefaultSpec);

    getExpCtx()->mongoProcessInterface =
        std::make_unique<MockMongoInterface>(std::vector<FieldPath>{{"x"}, {"_id"}});

    getExpCtx()->needsMerge = false;

    auto next = stages.back()->getNext();

    auto expectedSortKey = makeResumeToken(kDefaultTs, testUuid(), BSON("x" << 2 << "_id" << 1));

    ASSERT_TRUE(next.isAdvanced());
    ASSERT_VALUE_EQ(next.releaseDocument().metadata().getSortKey(), Value(expectedSortKey));
}

//
// Test class for change stream of a single database.
//
class ChangeStreamStageDBTest : public ChangeStreamStageTest {
public:
    ChangeStreamStageDBTest()
        : ChangeStreamStageTest(NamespaceString::makeCollectionlessAggregateNSS(nss.db())) {}
};

TEST_F(ChangeStreamStageDBTest, TransformInsert) {
    auto insert = makeOplogEntry(OpTypeEnum::kInsert, nss, BSON("_id" << 1 << "x" << 2));

    Document expectedInsert{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), BSON("x" << 2 << "_id" << 1))},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kFullDocumentField, D{{"_id", 1}, {"x", 2}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"x", 2}, {"_id", 1}}},  // Note _id <-> x reversal.
    };
    checkTransformation(insert, expectedInsert, {{"x"}, {"_id"}});
}

TEST_F(ChangeStreamStageDBTest, InsertOnOtherCollections) {
    NamespaceString otherNss("unittests.other_collection.");
    auto insertOtherColl =
        makeOplogEntry(OpTypeEnum::kInsert, otherNss, BSON("_id" << 1 << "x" << 2));

    // Insert on another collection in the same database.
    Document expectedInsert{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), BSON("x" << 2 << "_id" << 1))},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kFullDocumentField, D{{"_id", 1}, {"x", 2}}},
        {DSChangeStream::kNamespaceField, D{{"db", otherNss.db()}, {"coll", otherNss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"x", 2}, {"_id", 1}}},  // Note _id <-> x reversal.
    };
    checkTransformation(insertOtherColl, expectedInsert, {{"x"}, {"_id"}});
}

TEST_F(ChangeStreamStageDBTest, MatchFiltersChangesOnOtherDatabases) {
    std::set<NamespaceString> unmatchedNamespaces = {
        // Namespace starts with the db name, but is longer.
        NamespaceString("unittests2.coll"),
        // Namespace contains the db name, but not at the front.
        NamespaceString("test.unittests"),
        // Namespace contains the db name + dot.
        NamespaceString("test.unittests.coll"),
        // Namespace contains the db name + dot but is followed by $.
        NamespaceString("unittests.$cmd"),
    };

    // Insert into another database.
    for (auto& ns : unmatchedNamespaces) {
        auto insert = makeOplogEntry(OpTypeEnum::kInsert, ns, BSON("_id" << 1));
        checkTransformation(insert, boost::none);
    }
}

TEST_F(ChangeStreamStageDBTest, MatchFiltersAllSystemDotCollections) {
    auto nss = NamespaceString("unittests.system.coll");
    auto insert = makeOplogEntry(OpTypeEnum::kInsert, nss, BSON("_id" << 1));
    checkTransformation(insert, boost::none);

    nss = NamespaceString("unittests.system.users");
    insert = makeOplogEntry(OpTypeEnum::kInsert, nss, BSON("_id" << 1));
    checkTransformation(insert, boost::none);

    nss = NamespaceString("unittests.system.roles");
    insert = makeOplogEntry(OpTypeEnum::kInsert, nss, BSON("_id" << 1));
    checkTransformation(insert, boost::none);

    nss = NamespaceString("unittests.system.keys");
    insert = makeOplogEntry(OpTypeEnum::kInsert, nss, BSON("_id" << 1));
    checkTransformation(insert, boost::none);
}

TEST_F(ChangeStreamStageDBTest, TransformsEntriesForLegalClientCollectionsWithSystem) {
    std::set<NamespaceString> allowedNamespaces = {
        NamespaceString("unittests.coll.system"),
        NamespaceString("unittests.coll.system.views"),
        NamespaceString("unittests.systemx"),
    };

    for (auto& ns : allowedNamespaces) {
        auto insert = makeOplogEntry(OpTypeEnum::kInsert, ns, BSON("_id" << 1));
        Document expectedInsert{
            {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), BSON("_id" << 1))},
            {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
            {DSChangeStream::kClusterTimeField, kDefaultTs},
            {DSChangeStream::kFullDocumentField, D{{"_id", 1}}},
            {DSChangeStream::kNamespaceField, D{{"db", ns.db()}, {"coll", ns.coll()}}},
            {DSChangeStream::kDocumentKeyField, D{{"_id", 1}}},
        };
        checkTransformation(insert, expectedInsert, {{"_id"}});
    }
}

TEST_F(ChangeStreamStageDBTest, TransformUpdateFields) {
    BSONObj o = BSON("$set" << BSON("y" << 1));
    BSONObj o2 = BSON("_id" << 1 << "x" << 2);
    auto updateField = makeOplogEntry(OpTypeEnum::kUpdate, nss, o, testUuid(), boost::none, o2);

    Document expectedUpdateField{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), o2)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kUpdateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},
        {"updateDescription", D{{"updatedFields", D{{"y", 1}}}, {"removedFields", vector<V>()}}},
    };
    checkTransformation(updateField, expectedUpdateField);
}

TEST_F(ChangeStreamStageDBTest, TransformRemoveFields) {
    BSONObj o = BSON("$unset" << BSON("y" << 1));
    BSONObj o2 = BSON("_id" << 1 << "x" << 2);
    auto removeField = makeOplogEntry(OpTypeEnum::kUpdate,  // op type
                                      nss,                  // namespace
                                      o,                    // o
                                      testUuid(),           // uuid
                                      boost::none,          // fromMigrate
                                      o2);                  // o2

    // Remove fields
    Document expectedRemoveField{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), o2)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kUpdateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},
        {
            "updateDescription",
            D{{"updatedFields", D{}}, {"removedFields", {"y"_sd}}},
        }};
    checkTransformation(removeField, expectedRemoveField);
}

TEST_F(ChangeStreamStageDBTest, TransformReplace) {
    BSONObj o = BSON("_id" << 1 << "x" << 2 << "y" << 1);
    BSONObj o2 = BSON("_id" << 1 << "x" << 2);
    auto replace = makeOplogEntry(OpTypeEnum::kUpdate,  // op type
                                  nss,                  // namespace
                                  o,                    // o
                                  testUuid(),           // uuid
                                  boost::none,          // fromMigrate
                                  o2);                  // o2

    // Replace
    Document expectedReplace{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), o2)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kReplaceOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kFullDocumentField, D{{"_id", 1}, {"x", 2}, {"y", 1}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},
    };
    checkTransformation(replace, expectedReplace);
}

TEST_F(ChangeStreamStageDBTest, TransformDelete) {
    BSONObj o = BSON("_id" << 1 << "x" << 2);
    auto deleteEntry = makeOplogEntry(OpTypeEnum::kDelete,  // op type
                                      nss,                  // namespace
                                      o,                    // o
                                      testUuid(),           // uuid
                                      boost::none,          // fromMigrate
                                      boost::none);         // o2

    // Delete
    Document expectedDelete{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), o)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kDeleteOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},
    };
    checkTransformation(deleteEntry, expectedDelete);

    bool fromMigrate = false;  // also check actual "fromMigrate: false" not filtered
    auto deleteEntry2 = makeOplogEntry(deleteEntry.getOpType(),    // op type
                                       deleteEntry.getNss(),       // namespace
                                       deleteEntry.getObject(),    // o
                                       deleteEntry.getUuid(),      // uuid
                                       fromMigrate,                // fromMigrate
                                       deleteEntry.getObject2());  // o2

    checkTransformation(deleteEntry2, expectedDelete);
}

TEST_F(ChangeStreamStageDBTest, TransformDeleteFromMigrate) {
    bool fromMigrate = true;
    auto deleteEntry = makeOplogEntry(OpTypeEnum::kDelete,  // op type
                                      nss,                  // namespace
                                      BSON("_id" << 1),     // o
                                      boost::none,          // uuid
                                      fromMigrate,          // fromMigrate
                                      boost::none);         // o2

    checkTransformation(deleteEntry, boost::none);
}

TEST_F(ChangeStreamStageDBTest, TransformDeleteFromMigrateShowMigrations) {
    bool fromMigrate = true;
    BSONObj o = BSON("_id" << 1 << "x" << 2);
    auto deleteEntry = makeOplogEntry(OpTypeEnum::kDelete,  // op type
                                      nss,                  // namespace
                                      o,                    // o
                                      testUuid(),           // uuid
                                      fromMigrate,          // fromMigrate
                                      boost::none);         // o2

    // Delete
    auto spec = fromjson("{$changeStream: {showMigrationEvents: true}}");
    Document expectedDelete{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), o)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kDeleteOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},
    };

    checkTransformation(deleteEntry, expectedDelete, {}, spec);
}

TEST_F(ChangeStreamStageDBTest, TransformDrop) {
    OplogEntry dropColl = createCommand(BSON("drop" << nss.coll()), testUuid());
    Document expectedDrop{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid())},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kDropCollectionOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
    };
    checkTransformation(dropColl, expectedDrop);
}

TEST_F(ChangeStreamStageDBTest, TransformRename) {
    NamespaceString otherColl("test.bar");
    OplogEntry rename =
        createCommand(BSON("renameCollection" << nss.ns() << "to" << otherColl.ns()), testUuid());

    Document expectedRename{
        {DSChangeStream::kRenameTargetNssField,
         D{{"db", otherColl.db()}, {"coll", otherColl.coll()}}},
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid())},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kRenameCollectionOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
    };
    checkTransformation(rename, expectedRename);
}

TEST_F(ChangeStreamStageDBTest, TransformDropDatabase) {
    OplogEntry dropDB = createCommand(BSON("dropDatabase" << 1), boost::none, false);

    // Drop database entry doesn't have a UUID.
    Document expectedDropDatabase{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kDropDatabaseOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}}},
    };
    Document expectedInvalidate{
        {DSChangeStream::kIdField,
         makeResumeToken(
             kDefaultTs, Value(), Value(), ResumeTokenData::FromInvalidate::kFromInvalidate)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInvalidateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
    };

    checkTransformation(dropDB, expectedDropDatabase, {}, kDefaultSpec, expectedInvalidate);
}

TEST_F(ChangeStreamStageTest, TransformPreImageForDelete) {
    // Set the pre-image opTime to 1 second prior to the default event optime.
    repl::OpTime preImageOpTime{Timestamp(kDefaultTs.getSecs() - 1, 1), 1};
    const auto preImageObj = BSON("_id" << 1 << "x" << 2);

    // The documentKey for the main change stream event.
    const auto documentKey = BSON("_id" << 1);

    // The mock oplog UUID used by MockMongoInterface.
    auto oplogUUID = MockMongoInterface::oplogUuid();

    // Create an oplog entry for the pre-image no-op event.
    auto preImageEntry = makeOplogEntry(OpTypeEnum::kNoop,
                                        NamespaceString::kRsOplogNamespace,
                                        preImageObj,    // o
                                        oplogUUID,      // uuid
                                        boost::none,    // fromMigrate
                                        boost::none,    // o2
                                        preImageOpTime  // opTime
    );

    // Create an oplog entry for the delete event that will look up the pre-image.
    auto deleteEntry = makeOplogEntry(OpTypeEnum::kDelete,
                                      nss,
                                      documentKey,     // o
                                      testUuid(),      // uuid
                                      boost::none,     // fromMigrate
                                      boost::none,     // o2
                                      kDefaultOpTime,  // opTime
                                      {},              // sessionInfo
                                      {},              // prevOpTime
                                      preImageOpTime   // preImageOpTime
    );

    // Add the preImage oplog entry into a vector of documents that will be looked up. Add a dummy
    // entry before it so that we know we are finding the pre-image based on the given timestamp.
    repl::OpTime dummyOpTime{preImageOpTime.getTimestamp(), repl::OpTime::kInitialTerm};
    std::vector<Document> documentsForLookup = {Document{dummyOpTime.toBSON()},
                                                Document{preImageEntry.getEntry().toBSON()}};

    // When run with {fullDocumentBeforeChange: "off"}, we do not see a pre-image even if available.
    auto spec = BSON("$changeStream" << BSON("fullDocumentBeforeChange"
                                             << "off"));
    Document expectedDeleteNoPreImage{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), documentKey)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kDeleteOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, documentKey},
    };
    checkTransformation(
        deleteEntry, expectedDeleteNoPreImage, {}, spec, boost::none, {}, documentsForLookup);

    // When run with {fullDocumentBeforeChange: "whenAvailable"}, we see the pre-image.
    spec = BSON("$changeStream" << BSON("fullDocumentBeforeChange"
                                        << "whenAvailable"));
    Document expectedDeleteWithPreImage{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), documentKey)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kDeleteOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, documentKey},
        {DSChangeStream::kFullDocumentBeforeChangeField, preImageObj},
    };
    checkTransformation(
        deleteEntry, expectedDeleteWithPreImage, {}, spec, boost::none, {}, documentsForLookup);

    // When run with {fullDocumentBeforeChange: "required"}, we see the pre-image.
    spec = BSON("$changeStream" << BSON("fullDocumentBeforeChange"
                                        << "required"));
    checkTransformation(
        deleteEntry, expectedDeleteWithPreImage, {}, spec, boost::none, {}, documentsForLookup);

    // When run with {fullDocumentBeforeChange: "whenAvailable"} but no pre-image is available, the
    // output 'fullDocumentBeforeChange' field is explicitly set to 'null'.
    spec = BSON("$changeStream" << BSON("fullDocumentBeforeChange"
                                        << "whenAvailable"));
    MutableDocument expectedDeleteWithNullPreImage(expectedDeleteNoPreImage);
    expectedDeleteWithNullPreImage.addField(DSChangeStream::kFullDocumentBeforeChangeField,
                                            Value(BSONNULL));
    checkTransformation(deleteEntry, expectedDeleteWithNullPreImage.freeze(), {}, spec);

    // When run with {fullDocumentBeforeChange: "required"} but we cannot find the pre-image, we
    // throw NoMatchingDocument.
    spec = BSON("$changeStream" << BSON("fullDocumentBeforeChange"
                                        << "required"));
    ASSERT_THROWS_CODE(checkTransformation(deleteEntry, boost::none, {}, spec),
                       AssertionException,
                       ErrorCodes::NoMatchingDocument);
}

TEST_F(ChangeStreamStageTest, TransformPreImageForUpdate) {
    // Set the pre-image opTime to 1 second prior to the default event optime.
    repl::OpTime preImageOpTime{Timestamp(kDefaultTs.getSecs() - 1, 1), 1};

    // Define the pre-image object, the update operation spec, and the document key.
    const auto updateSpec = BSON("$unset" << BSON("x" << 1));
    const auto preImageObj = BSON("_id" << 1 << "x" << 2);
    const auto documentKey = BSON("_id" << 1);

    // The mock oplog UUID used by MockMongoInterface.
    auto oplogUUID = MockMongoInterface::oplogUuid();

    // Create an oplog entry for the pre-image no-op event.
    auto preImageEntry = makeOplogEntry(OpTypeEnum::kNoop,
                                        NamespaceString::kRsOplogNamespace,
                                        preImageObj,    // o
                                        oplogUUID,      // uuid
                                        boost::none,    // fromMigrate
                                        boost::none,    // o2
                                        preImageOpTime  // opTime
    );

    // Create an oplog entry for the update event that will look up the pre-image.
    auto updateEntry = makeOplogEntry(OpTypeEnum::kUpdate,
                                      nss,
                                      updateSpec,      // o
                                      testUuid(),      // uuid
                                      boost::none,     // fromMigrate
                                      documentKey,     // o2
                                      kDefaultOpTime,  // opTime
                                      {},              // sessionInfo
                                      {},              // prevOpTime
                                      preImageOpTime   // preImageOpTime
    );

    // Add the preImage oplog entry into a vector of documents that will be looked up. Add a dummy
    // entry before it so that we know we are finding the pre-image based on the given timestamp.
    repl::OpTime dummyOpTime{preImageOpTime.getTimestamp(), repl::OpTime::kInitialTerm};
    std::vector<Document> documentsForLookup = {Document{dummyOpTime.toBSON()},
                                                Document{preImageEntry.getEntry().toBSON()}};

    // When run with {fullDocumentBeforeChange: "off"}, we do not see a pre-image even if available.
    auto spec = BSON("$changeStream" << BSON("fullDocumentBeforeChange"
                                             << "off"));
    Document expectedUpdateNoPreImage{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), documentKey)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kUpdateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, documentKey},
        {
            "updateDescription",
            D{{"updatedFields", D{}}, {"removedFields", vector<V>{V("x"_sd)}}},
        },
    };
    checkTransformation(
        updateEntry, expectedUpdateNoPreImage, {}, spec, boost::none, {}, documentsForLookup);

    // When run with {fullDocumentBeforeChange: "whenAvailable"}, we see the pre-image.
    spec = BSON("$changeStream" << BSON("fullDocumentBeforeChange"
                                        << "whenAvailable"));
    Document expectedUpdateWithPreImage{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), documentKey)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kUpdateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, documentKey},
        {
            "updateDescription",
            D{{"updatedFields", D{}}, {"removedFields", vector<V>{V("x"_sd)}}},
        },
        {DSChangeStream::kFullDocumentBeforeChangeField, preImageObj},
    };
    checkTransformation(
        updateEntry, expectedUpdateWithPreImage, {}, spec, boost::none, {}, documentsForLookup);

    // When run with {fullDocumentBeforeChange: "required"}, we see the pre-image.
    spec = BSON("$changeStream" << BSON("fullDocumentBeforeChange"
                                        << "required"));
    checkTransformation(
        updateEntry, expectedUpdateWithPreImage, {}, spec, boost::none, {}, documentsForLookup);

    // When run with {fullDocumentBeforeChange: "whenAvailable"} but no pre-image is available, the
    // output 'fullDocumentBeforeChange' field is explicitly set to 'null'.
    spec = BSON("$changeStream" << BSON("fullDocumentBeforeChange"
                                        << "whenAvailable"));
    MutableDocument expectedUpdateWithNullPreImage(expectedUpdateNoPreImage);
    expectedUpdateWithNullPreImage.addField(DSChangeStream::kFullDocumentBeforeChangeField,
                                            Value(BSONNULL));
    checkTransformation(updateEntry, expectedUpdateWithNullPreImage.freeze(), {}, spec);

    // When run with {fullDocumentBeforeChange: "required"} but we cannot find the pre-image, we
    // throw NoMatchingDocument.
    spec = BSON("$changeStream" << BSON("fullDocumentBeforeChange"
                                        << "required"));
    ASSERT_THROWS_CODE(checkTransformation(updateEntry, boost::none, {}, spec),
                       AssertionException,
                       ErrorCodes::NoMatchingDocument);
}

TEST_F(ChangeStreamStageTest, TransformPreImageForReplace) {
    // Set the pre-image opTime to 1 second prior to the default event optime.
    repl::OpTime preImageOpTime{Timestamp(kDefaultTs.getSecs() - 1, 1), 1};

    // Define the pre-image object, the replacement document, and the document key.
    const auto replacementDoc = BSON("_id" << 1 << "y" << 3);
    const auto preImageObj = BSON("_id" << 1 << "x" << 2);
    const auto documentKey = BSON("_id" << 1);

    // The mock oplog UUID used by MockMongoInterface.
    auto oplogUUID = MockMongoInterface::oplogUuid();

    // Create an oplog entry for the pre-image no-op event.
    auto preImageEntry = makeOplogEntry(OpTypeEnum::kNoop,
                                        NamespaceString::kRsOplogNamespace,
                                        preImageObj,    // o
                                        oplogUUID,      // uuid
                                        boost::none,    // fromMigrate
                                        boost::none,    // o2
                                        preImageOpTime  // opTime
    );

    // Create an oplog entry for the replacement event that will look up the pre-image.
    auto replaceEntry = makeOplogEntry(OpTypeEnum::kUpdate,
                                       nss,
                                       replacementDoc,  // o
                                       testUuid(),      // uuid
                                       boost::none,     // fromMigrate
                                       documentKey,     // o2
                                       kDefaultOpTime,  // opTime
                                       {},              // sessionInfo
                                       {},              // prevOpTime
                                       preImageOpTime   // preImageOpTime
    );

    // Add the preImage oplog entry into a vector of documents that will be looked up. Add a dummy
    // entry before it so that we know we are finding the pre-image based on the given timestamp.
    repl::OpTime dummyOpTime{preImageOpTime.getTimestamp(), repl::OpTime::kInitialTerm};
    std::vector<Document> documentsForLookup = {Document{dummyOpTime.toBSON()},
                                                Document{preImageEntry.getEntry().toBSON()}};

    // When run with {fullDocumentBeforeChange: "off"}, we do not see a pre-image even if available.
    auto spec = BSON("$changeStream" << BSON("fullDocumentBeforeChange"
                                             << "off"));
    Document expectedReplaceNoPreImage{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), documentKey)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kReplaceOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kFullDocumentField, replacementDoc},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, documentKey},
    };
    checkTransformation(
        replaceEntry, expectedReplaceNoPreImage, {}, spec, boost::none, {}, documentsForLookup);

    // When run with {fullDocumentBeforeChange: "whenAvailable"}, we see the pre-image.
    spec = BSON("$changeStream" << BSON("fullDocumentBeforeChange"
                                        << "whenAvailable"));
    Document expectedReplaceWithPreImage{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), documentKey)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kReplaceOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kFullDocumentField, replacementDoc},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, documentKey},
        {DSChangeStream::kFullDocumentBeforeChangeField, preImageObj},
    };
    checkTransformation(
        replaceEntry, expectedReplaceWithPreImage, {}, spec, boost::none, {}, documentsForLookup);

    // When run with {fullDocumentBeforeChange: "required"}, we see the pre-image.
    spec = BSON("$changeStream" << BSON("fullDocumentBeforeChange"
                                        << "required"));
    checkTransformation(
        replaceEntry, expectedReplaceWithPreImage, {}, spec, boost::none, {}, documentsForLookup);

    // When run with {fullDocumentBeforeChange: "whenAvailable"} but no pre-image is available, the
    // output 'fullDocumentBeforeChange' field is explicitly set to 'null'.
    spec = BSON("$changeStream" << BSON("fullDocumentBeforeChange"
                                        << "whenAvailable"));
    MutableDocument expectedReplaceWithNullPreImage(expectedReplaceNoPreImage);
    expectedReplaceWithNullPreImage.addField(DSChangeStream::kFullDocumentBeforeChangeField,
                                             Value(BSONNULL));
    checkTransformation(replaceEntry, expectedReplaceWithNullPreImage.freeze(), {}, spec);

    // When run with {fullDocumentBeforeChange: "required"} but we cannot find the pre-image, we
    // throw NoMatchingDocument.
    spec = BSON("$changeStream" << BSON("fullDocumentBeforeChange"
                                        << "required"));
    ASSERT_THROWS_CODE(checkTransformation(replaceEntry, boost::none, {}, spec),
                       AssertionException,
                       ErrorCodes::NoMatchingDocument);
}

TEST_F(ChangeStreamStageDBTest, MatchFiltersOperationsOnSystemCollections) {
    NamespaceString systemColl(nss.db() + ".system.users");
    OplogEntry insert = makeOplogEntry(OpTypeEnum::kInsert, systemColl, BSON("_id" << 1));
    checkTransformation(insert, boost::none);

    OplogEntry dropColl = createCommand(BSON("drop" << systemColl.coll()), testUuid());
    checkTransformation(dropColl, boost::none);

    // Rename from a 'system' collection to another 'system' collection should not include a
    // notification.
    NamespaceString renamedSystemColl(nss.db() + ".system.views");
    OplogEntry rename = createCommand(
        BSON("renameCollection" << systemColl.ns() << "to" << renamedSystemColl.ns()), testUuid());
    checkTransformation(rename, boost::none);
}

TEST_F(ChangeStreamStageDBTest, RenameFromSystemToUserCollectionShouldIncludeNotification) {
    // Renaming to a non-system collection will include a notification in the stream.
    NamespaceString systemColl(nss.db() + ".system.users");
    NamespaceString renamedColl(nss.db() + ".non_system_coll");
    OplogEntry rename = createCommand(
        BSON("renameCollection" << systemColl.ns() << "to" << renamedColl.ns()), testUuid());

    // Note that the collection rename does *not* have the queued invalidated field.
    Document expectedRename{
        {DSChangeStream::kRenameTargetNssField,
         D{{"db", renamedColl.db()}, {"coll", renamedColl.coll()}}},
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid())},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kRenameCollectionOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", systemColl.db()}, {"coll", systemColl.coll()}}},
    };
    checkTransformation(rename, expectedRename);
}

TEST_F(ChangeStreamStageDBTest, RenameFromUserToSystemCollectionShouldIncludeNotification) {
    // Renaming to a system collection will include a notification in the stream.
    NamespaceString systemColl(nss.db() + ".system.users");
    OplogEntry rename =
        createCommand(BSON("renameCollection" << nss.ns() << "to" << systemColl.ns()), testUuid());

    // Note that the collection rename does *not* have the queued invalidated field.
    Document expectedRename{
        {DSChangeStream::kRenameTargetNssField,
         D{{"db", systemColl.db()}, {"coll", systemColl.coll()}}},
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid())},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kRenameCollectionOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
    };
    checkTransformation(rename, expectedRename);
}

TEST_F(ChangeStreamStageDBTest, MatchFiltersNoOp) {
    OplogEntry noOp = makeOplogEntry(OpTypeEnum::kNoop,
                                     NamespaceString(),
                                     BSON(repl::ReplicationCoordinator::newPrimaryMsgField
                                          << repl::ReplicationCoordinator::newPrimaryMsg));
    checkTransformation(noOp, boost::none);
}

TEST_F(ChangeStreamStageDBTest, DocumentKeyShouldIncludeShardKeyFromResumeToken) {
    const Timestamp ts(3, 45);
    const long long term = 4;
    const auto opTime = repl::OpTime(ts, term);
    const auto uuid = testUuid();

    std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);
    CollectionCatalog::write(getExpCtx()->opCtx, [&](CollectionCatalog& catalog) {
        catalog.registerCollection(getExpCtx()->opCtx, uuid, std::move(collection));
    });

    BSONObj o2 = BSON("_id" << 1 << "shardKey" << 2);
    auto resumeToken = makeResumeToken(ts, uuid, o2);

    BSONObj insertDoc = BSON("_id" << 2 << "shardKey" << 3);
    auto insertEntry = makeOplogEntry(OpTypeEnum::kInsert,  // op type
                                      nss,                  // namespace
                                      insertDoc,            // o
                                      uuid,                 // uuid
                                      boost::none,          // fromMigrate
                                      boost::none,          // o2
                                      opTime);              // opTime

    Document expectedInsert{
        {DSChangeStream::kIdField, makeResumeToken(ts, uuid, insertDoc)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, ts},
        {DSChangeStream::kFullDocumentField, D{{"_id", 2}, {"shardKey", 3}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 2}, {"shardKey", 3}}},
    };
    checkTransformation(
        insertEntry,
        expectedInsert,
        {{"_id"}},  // Mock the 'collectDocumentKeyFieldsForHostedCollection' response.
        BSON("$changeStream" << BSON("resumeAfter" << resumeToken)));
}

TEST_F(ChangeStreamStageDBTest, DocumentKeyShouldNotIncludeShardKeyFieldsIfNotPresentInOplogEntry) {
    const Timestamp ts(3, 45);
    const long long term = 4;
    const auto opTime = repl::OpTime(ts, term);
    const auto uuid = testUuid();

    std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);
    CollectionCatalog::write(getExpCtx()->opCtx, [&](CollectionCatalog& catalog) {
        catalog.registerCollection(getExpCtx()->opCtx, uuid, std::move(collection));
    });

    BSONObj o2 = BSON("_id" << 1 << "shardKey" << 2);
    auto resumeToken = makeResumeToken(ts, uuid, o2);

    // Note that the 'o' field in the oplog entry does not contain the shard key field.
    BSONObj insertDoc = BSON("_id" << 2);
    auto insertEntry = makeOplogEntry(OpTypeEnum::kInsert,  // op type
                                      nss,                  // namespace
                                      insertDoc,            // o
                                      uuid,                 // uuid
                                      boost::none,          // fromMigrate
                                      boost::none,          // o2
                                      opTime);              // opTime

    Document expectedInsert{
        {DSChangeStream::kIdField, makeResumeToken(ts, uuid, insertDoc)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, ts},
        {DSChangeStream::kFullDocumentField, D{{"_id", 2}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 2}}},
    };
    checkTransformation(
        insertEntry,
        expectedInsert,
        {{"_id"}},  // Mock the 'collectDocumentKeyFieldsForHostedCollection' response.
        BSON("$changeStream" << BSON("resumeAfter" << resumeToken)));
}

TEST_F(ChangeStreamStageDBTest, DocumentKeyShouldNotIncludeShardKeyIfResumeTokenDoesntContainUUID) {
    const Timestamp ts(3, 45);
    const long long term = 4;
    const auto opTime = repl::OpTime(ts, term);
    const auto uuid = testUuid();

    std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);
    CollectionCatalog::write(getExpCtx()->opCtx, [&](CollectionCatalog& catalog) {
        catalog.registerCollection(getExpCtx()->opCtx, uuid, std::move(collection));
    });

    // Create a resume token from only the timestamp.
    auto resumeToken = makeResumeToken(ts);

    // Insert oplog entry contains shardKey, however we are not able to extract the shard key from
    // the resume token.
    BSONObj insertDoc = BSON("_id" << 2 << "shardKey" << 3);
    auto insertEntry = makeOplogEntry(OpTypeEnum::kInsert,  // op type
                                      nss,                  // namespace
                                      insertDoc,            // o
                                      uuid,                 // uuid
                                      boost::none,          // fromMigrate
                                      boost::none,          // o2
                                      opTime);              // opTime

    Document expectedInsert{
        {DSChangeStream::kIdField, makeResumeToken(ts, uuid, BSON("_id" << 2))},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, ts},
        {DSChangeStream::kFullDocumentField, D{{"_id", 2}, {"shardKey", 3}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 2}}},
    };
    checkTransformation(
        insertEntry,
        expectedInsert,
        {{"_id"}},  // Mock the 'collectDocumentKeyFieldsForHostedCollection' response.
        BSON("$changeStream" << BSON("resumeAfter" << resumeToken)));
}

TEST_F(ChangeStreamStageDBTest, ResumeAfterWithTokenFromInvalidateShouldFail) {
    auto expCtx = getExpCtx();

    // Need to put the collection in the collection catalog so the resume token is valid.
    std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);
    CollectionCatalog::write(expCtx->opCtx, [&](CollectionCatalog& catalog) {
        catalog.registerCollection(getExpCtx()->opCtx, testUuid(), std::move(collection));
    });

    const auto resumeTokenInvalidate =
        makeResumeToken(kDefaultTs,
                        testUuid(),
                        BSON("x" << 2 << "_id" << 1),
                        ResumeTokenData::FromInvalidate::kFromInvalidate);

    ASSERT_THROWS_CODE(
        DSChangeStream::createFromBson(
            BSON(DSChangeStream::kStageName << BSON("resumeAfter" << resumeTokenInvalidate))
                .firstElement(),
            expCtx),
        AssertionException,
        ErrorCodes::InvalidResumeToken);
}

TEST_F(ChangeStreamStageDBTest, ResumeAfterWithTokenFromDropDatabase) {
    const auto uuid = testUuid();

    std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);
    CollectionCatalog::write(getExpCtx()->opCtx, [&](CollectionCatalog& catalog) {
        catalog.registerCollection(getExpCtx()->opCtx, uuid, std::move(collection));
    });

    // Create a resume token from only the timestamp, similar to a 'dropDatabase' entry.
    auto resumeToken = makeResumeToken(
        kDefaultTs, Value(), Value(), ResumeTokenData::FromInvalidate::kNotFromInvalidate);

    BSONObj insertDoc = BSON("_id" << 2);
    auto insertEntry = makeOplogEntry(OpTypeEnum::kInsert, nss, insertDoc);

    Document expectedInsert{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), insertDoc)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kFullDocumentField, D{{"_id", 2}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 2}}},
    };
    checkTransformation(
        insertEntry,
        expectedInsert,
        {{"_id"}},  // Mock the 'collectDocumentKeyFieldsForHostedCollection' response.
        BSON("$changeStream" << BSON("resumeAfter" << resumeToken)));
}


TEST_F(ChangeStreamStageDBTest, StartAfterSucceedsEvenIfResumeTokenDoesNotContainUUID) {
    const auto uuid = testUuid();

    std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);
    CollectionCatalog::write(getExpCtx()->opCtx, [&](CollectionCatalog& catalog) {
        catalog.registerCollection(getExpCtx()->opCtx, uuid, std::move(collection));
    });

    // Create a resume token from only the timestamp, similar to a 'dropDatabase' entry.
    auto resumeToken = makeResumeToken(kDefaultTs);

    BSONObj insertDoc = BSON("_id" << 2);
    auto insertEntry = makeOplogEntry(OpTypeEnum::kInsert, nss, insertDoc);

    Document expectedInsert{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, uuid, insertDoc)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kFullDocumentField, D{{"_id", 2}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 2}}},
    };
    checkTransformation(
        insertEntry,
        expectedInsert,
        {{"_id"}},  // Mock the 'collectDocumentKeyFieldsForHostedCollection' response.
        BSON("$changeStream" << BSON("startAfter" << resumeToken)));
}

TEST_F(ChangeStreamStageTest, ChangeStreamWithSingleMatch) {
    //
    // Tests that the single '$match' gets promoted before the '$_internalUpdateOnAddShard'.
    //
    const std::vector<BSONObj> rawPipeline = {
        fromjson("{$changeStream: {}}"),
        fromjson("{$match: {operationType: 'insert'}}"),
    };

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$match",
                           "$_internalChangeStreamHandleTopologyChange"});
}

TEST_F(ChangeStreamStageTest, ChangeStreamWithMultipleMatch) {
    //
    // Tests that multiple '$match' gets merged and promoted before the
    // '$_internalUpdateOnAddShard'.
    //
    const std::vector<BSONObj> rawPipeline = {fromjson("{$changeStream: {}}"),
                                              fromjson("{$match: {operationType: 'insert'}}"),
                                              fromjson("{$match: {operationType: 'delete'}}")};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$match",
                           "$_internalChangeStreamHandleTopologyChange"});
}

TEST_F(ChangeStreamStageTest, ChangeStreamWithMultipleMatchAndResumeToken) {
    //
    // Tests that multiple '$match' gets merged and promoted before the
    // '$_internalUpdateOnAddShard' if resume token if present.
    //
    auto resumeToken = makeResumeToken(kDefaultTs, testUuid());

    const std::vector<BSONObj> rawPipeline = {
        BSON("$changeStream" << BSON("resumeAfter" << resumeToken)),
        BSON("$match" << BSON("operationType"
                              << "insert")),
        BSON("$match" << BSON("operationType"
                              << "insert"))};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$match",
                           "$_internalChangeStreamHandleTopologyChange",
                           "$_internalChangeStreamEnsureResumeTokenPresent"});
}

TEST_F(ChangeStreamStageTest, ChangeStreamWithSingleProject) {
    //
    // Tests that the single'$project' gets promoted before the '$_internalUpdateOnAddShard'.
    //
    const std::vector<BSONObj> rawPipeline = {fromjson("{$changeStream: {}}"),
                                              fromjson("{$project: {operationType: 1}}")};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$project",
                           "$_internalChangeStreamHandleTopologyChange"});
}

TEST_F(ChangeStreamStageTest, ChangeStreamWithMultipleProject) {
    //
    // Tests that multiple '$project' gets promoted before the '$_internalUpdateOnAddShard'.
    //
    const std::vector<BSONObj> rawPipeline = {fromjson("{$changeStream: {}}"),
                                              fromjson("{$project: {operationType: 1}}"),
                                              fromjson("{$project: {fullDocument: 1}}")};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$project",
                           "$project",
                           "$_internalChangeStreamHandleTopologyChange"});
}

TEST_F(ChangeStreamStageTest, ChangeStreamWithMultipleProjectAndResumeToken) {
    //
    // Tests that multiple '$project' gets promoted before the '$_internalUpdateOnAddShard' if
    // resume token is present.
    //
    auto resumeToken = makeResumeToken(kDefaultTs, testUuid());

    const std::vector<BSONObj> rawPipeline = {
        BSON("$changeStream" << BSON("resumeAfter" << resumeToken)),
        BSON("$project" << BSON("operationType" << 1)),
        BSON("$project" << BSON("fullDocument" << 1))};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$project",
                           "$project",
                           "$_internalChangeStreamHandleTopologyChange",
                           "$_internalChangeStreamEnsureResumeTokenPresent"});
}

TEST_F(ChangeStreamStageTest, ChangeStreamWithProjectMatchAndResumeToken) {
    //
    // Tests that a '$project' followed by a '$match' gets optimized and they get promoted before
    // the '$_internalUpdateOnAddShard'.
    //
    auto resumeToken = makeResumeToken(kDefaultTs, testUuid());

    const std::vector<BSONObj> rawPipeline = {
        BSON("$changeStream" << BSON("resumeAfter" << resumeToken)),
        BSON("$project" << BSON("operationType" << 1)),
        BSON("$match" << BSON("operationType"
                              << "insert"))};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$match",
                           "$project",
                           "$_internalChangeStreamHandleTopologyChange",
                           "$_internalChangeStreamEnsureResumeTokenPresent"});
}

TEST_F(ChangeStreamStageTest, ChangeStreamWithSingleUnset) {
    //
    // Tests that the single'$unset' gets promoted before the '$_internalUpdateOnAddShard' as
    // '$project'.
    //
    const std::vector<BSONObj> rawPipeline = {fromjson("{$changeStream: {}}"),
                                              fromjson("{$unset: 'operationType'}")};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$project",
                           "$_internalChangeStreamHandleTopologyChange"});
}

TEST_F(ChangeStreamStageTest, ChangeStreamWithMultipleUnset) {
    //
    // Tests that multiple '$unset' gets promoted before the '$_internalUpdateOnAddShard' as
    // '$project'.
    //
    const std::vector<BSONObj> rawPipeline = {fromjson("{$changeStream: {}}"),
                                              fromjson("{$unset: 'operationType'}"),
                                              fromjson("{$unset: 'fullDocument'}")};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$project",
                           "$project",
                           "$_internalChangeStreamHandleTopologyChange"});
}

TEST_F(ChangeStreamStageTest, ChangeStreamWithUnsetAndResumeToken) {
    //
    // Tests that the '$unset' gets promoted before the '$_internalUpdateOnAddShard' as '$project'
    // even if resume token is present.
    //
    auto resumeToken = makeResumeToken(kDefaultTs, testUuid());

    const std::vector<BSONObj> rawPipeline = {BSON("$changeStream"
                                                   << BSON("resumeAfter" << resumeToken)),
                                              BSON("$unset"
                                                   << "operationType")};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$project",
                           "$_internalChangeStreamHandleTopologyChange",
                           "$_internalChangeStreamEnsureResumeTokenPresent"});
}

TEST_F(ChangeStreamStageTest, ChangeStreamWithSingleAddFields) {
    //
    // Tests that the single'$addFields' gets promoted before the '$_internalUpdateOnAddShard'.
    //
    const std::vector<BSONObj> rawPipeline = {fromjson("{$changeStream: {}}"),
                                              fromjson("{$addFields: {stockPrice: 100}}")};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$addFields",
                           "$_internalChangeStreamHandleTopologyChange"});
}

TEST_F(ChangeStreamStageTest, ChangeStreamWithMultipleAddFields) {
    //
    // Tests that multiple '$addFields' gets promoted before the '$_internalUpdateOnAddShard'.
    //
    const std::vector<BSONObj> rawPipeline = {fromjson("{$changeStream: {}}"),
                                              fromjson("{$addFields: {stockPrice: 100}}"),
                                              fromjson("{$addFields: {quarter: 'Q1'}}")};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$addFields",
                           "$addFields",
                           "$_internalChangeStreamHandleTopologyChange"});
}

TEST_F(ChangeStreamStageTest, ChangeStreamWithAddFieldsAndResumeToken) {
    //
    // Tests that the '$addFields' gets promoted before the '$_internalUpdateOnAddShard' if
    // resume token is present.
    //
    auto resumeToken = makeResumeToken(kDefaultTs, testUuid());

    const std::vector<BSONObj> rawPipeline = {
        BSON("$changeStream" << BSON("resumeAfter" << resumeToken)),
        BSON("$addFields" << BSON("stockPrice" << 100))};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$addFields",
                           "$_internalChangeStreamHandleTopologyChange",
                           "$_internalChangeStreamEnsureResumeTokenPresent"});
}

TEST_F(ChangeStreamStageTest, ChangeStreamWithSingleSet) {
    //
    // Tests that the single'$set' gets promoted before the '$_internalUpdateOnAddShard'.
    //
    const std::vector<BSONObj> rawPipeline = {fromjson("{$changeStream: {}}"),
                                              fromjson("{$set: {stockPrice: 100}}")};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$set",
                           "$_internalChangeStreamHandleTopologyChange"});
}

TEST_F(ChangeStreamStageTest, ChangeStreamWithMultipleSet) {
    //
    // Tests that multiple '$set' gets promoted before the '$_internalUpdateOnAddShard'.
    //
    const std::vector<BSONObj> rawPipeline = {fromjson("{$changeStream: {}}"),
                                              fromjson("{$set: {stockPrice: 100}}"),
                                              fromjson("{$set: {quarter: 'Q1'}}")};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$set",
                           "$set",
                           "$_internalChangeStreamHandleTopologyChange"});
}

TEST_F(ChangeStreamStageTest, ChangeStreamWithSetAndResumeToken) {
    //
    // Tests that the '$set' gets promoted before the '$_internalUpdateOnAddShard' if
    // resume token is present.
    //
    auto resumeToken = makeResumeToken(kDefaultTs, testUuid());

    const std::vector<BSONObj> rawPipeline = {
        BSON("$changeStream" << BSON("resumeAfter" << resumeToken)),
        BSON("$set" << BSON("stockPrice" << 100))};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$set",
                           "$_internalChangeStreamHandleTopologyChange",
                           "$_internalChangeStreamEnsureResumeTokenPresent"});
}

TEST_F(ChangeStreamStageTest, ChangeStreamWithSingleReplaceRoot) {
    //
    // Tests that the single'$replaceRoot' gets promoted before the '$_internalUpdateOnAddShard'.
    //
    const std::vector<BSONObj> rawPipeline = {
        fromjson("{$changeStream: {}}"), fromjson("{$replaceRoot: {newRoot: '$fullDocument'}}")};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$replaceRoot",
                           "$_internalChangeStreamHandleTopologyChange"});
}

TEST_F(ChangeStreamStageTest, ChangeStreamWithReplaceRootAndResumeToken) {
    //
    // Tests that the '$replaceRoot' gets promoted before the '$_internalUpdateOnAddShard' if
    // resume token is present.
    //
    auto resumeToken = makeResumeToken(kDefaultTs, testUuid());

    const std::vector<BSONObj> rawPipeline = {
        BSON("$changeStream" << BSON("resumeAfter" << resumeToken)),
        BSON("$replaceRoot" << BSON("newRoot"
                                    << "$fullDocument"))};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$replaceRoot",
                           "$_internalChangeStreamHandleTopologyChange",
                           "$_internalChangeStreamEnsureResumeTokenPresent"});
}

TEST_F(ChangeStreamStageTest, ChangeStreamWithSingleReplaceWith) {
    //
    // Tests that the single '$replaceWith' gets promoted before the '$_internalUpdateOnAddShard' as
    // '$replaceRoot'.
    //
    const std::vector<BSONObj> rawPipeline = {fromjson("{$changeStream: {}}"),
                                              fromjson("{$replaceWith: '$fullDocument'}")};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$replaceRoot",
                           "$_internalChangeStreamHandleTopologyChange"});
}

TEST_F(ChangeStreamStageTest, ChangeStreamWithReplaceWithAndResumeToken) {
    //
    // Tests that the '$replaceWith' gets promoted before the '$_internalUpdateOnAddShard' if
    // resume token is present as '$replaceRoot'.
    //
    auto resumeToken = makeResumeToken(kDefaultTs, testUuid());

    const std::vector<BSONObj> rawPipeline = {BSON("$changeStream"
                                                   << BSON("resumeAfter" << resumeToken)),
                                              BSON("$replaceWith"
                                                   << "$fullDocument")};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$replaceRoot",
                           "$_internalChangeStreamHandleTopologyChange",
                           "$_internalChangeStreamEnsureResumeTokenPresent"});
}

TEST_F(ChangeStreamStageTest, ChangeStreamWithAllStagesAndResumeToken) {
    //
    // Tests that when all allowed stages are included along with the resume token, the final
    // pipeline gets optimized.
    //
    auto resumeToken = makeResumeToken(kDefaultTs, testUuid());

    const std::vector<BSONObj> rawPipeline = {BSON("$changeStream"
                                                   << BSON("resumeAfter" << resumeToken)),
                                              BSON("$project" << BSON("operationType" << 1)),
                                              BSON("$unset"
                                                   << "_id"),
                                              BSON("$addFields" << BSON("stockPrice" << 100)),
                                              BSON("$set"
                                                   << BSON("fullDocument.stockPrice" << 100)),
                                              BSON("$match" << BSON("operationType"
                                                                    << "insert")),
                                              BSON("$replaceRoot" << BSON("newRoot"
                                                                          << "$fullDocument")),
                                              BSON("$replaceWith"
                                                   << "fullDocument.stockPrice")};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$match",
                           "$project",
                           "$project",
                           "$addFields",
                           "$set",
                           "$replaceRoot",
                           "$replaceRoot",
                           "$_internalChangeStreamHandleTopologyChange",
                           "$_internalChangeStreamEnsureResumeTokenPresent"});
}

class ChangeStreamRewriteTest : public AggregationContextFixture {
public:
    std::string getNsDbRegexMatchExpr(const std::string& field, const std::string& regex) {
        return str::stream()
            << "{$expr: {$let: {vars: {oplogField: {$cond: [{ $eq: [{ $type: ['" << field
            << "']}, {$const: 'string'}]}, '" << field
            << "', '$$REMOVE']}}, in: {$regexMatch: {input: {$substrBytes: ['$$oplogField', "
               "{$const: 0}, {$ifNull: [{$indexOfBytes: ['$$oplogField', {$const: "
               "'.'}]}, {$const: 0}]}]}, regex: {$const: '"
            << regex << "'}, options: {$const: ''}}}}}}";
    }

    std::string getNsCollRegexMatchExpr(const std::string& field, const std::string& regex) {
        if (field == "$o.drop") {
            return str::stream()
                << "{$expr: {$let: {vars: {oplogField: {$cond: [{ $eq: [{ $type: ['" << field
                << "']}, {$const: 'string'}]}, '" << field
                << "', '$$REMOVE']}}, in: {$regexMatch: {input: '$$oplogField', regex: {$const: '"
                << regex << "'}, options: {$const: ''}}}}}}";
        }

        return str::stream()
            << "{$expr: {$let: {vars: {oplogField: {$cond: [{ $eq: [{ $type: ['" << field
            << "']}, {$const: 'string'}]}, '" << field
            << "', '$$REMOVE']}}, in: {$regexMatch: {input: {$substrBytes: ['$$oplogField', {$add: "
               "[{$const: 1}, "
               "{$ifNull: [{$indexOfBytes: ['$$oplogField', {$const: '.'}]}, {$const: 0}]}]}, "
               "{$const: -1}]}, regex: {$const: '"
            << regex << "'}, options: {$const: ''}} }}}}";
    }
};

//
// Logical rewrites
//
TEST_F(ChangeStreamRewriteTest, RewriteOrPredicateOnRenameableFields) {
    auto spec = fromjson("{$or: [{clusterTime: {$type: [17]}}, {lsid: {$type: [16]}}]}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"clusterTime", "lsid"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$or: [{ts: {$type: [17]}}, {lsid: {$type: [16]}}]}"));
}

TEST_F(ChangeStreamRewriteTest, InexactRewriteOfAndWithUnrewritableChild) {
    // Note that rewrite of {operationType: {$gt: ...}} is currently not supported.
    auto spec = fromjson("{$and: [{lsid: {$type: [16]}}, {operationType: {$gt: 0}}]}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression =
        change_stream_rewrite::rewriteFilterForFields(getExpCtx(),
                                                      statusWithMatchExpression.getValue().get(),
                                                      {"clusterTime", "lsid", "operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, fromjson("{$and: [{lsid: {$type: [16]}}]}"));
}

TEST_F(ChangeStreamRewriteTest, CannotRewriteOrWithUnrewritableChild) {
    // Note that rewrite of {operationType: {$gt: ...}} is currently not supported.
    auto spec = fromjson(
        "{$and: [{clusterTime: {$type: [17]}}, "
        "{$or: [{lsid: {$type: [16]}}, {operationType: {$gt: 0}}]}]}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression =
        change_stream_rewrite::rewriteFilterForFields(getExpCtx(),
                                                      statusWithMatchExpression.getValue().get(),
                                                      {"clusterTime", "lsid", "operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, fromjson("{$and: [{ts: {$type: [17]}}]}"));
}

TEST_F(ChangeStreamRewriteTest, InexactRewriteOfNorWithUnrewritableChild) {
    // Note that rewrite of {operationType: {$gt: ...}} is currently not supported.
    auto spec = fromjson(
        "{$and: [{clusterTime: {$type: [17]}}, "
        "{$nor: [{lsid: {$type: [16]}}, {operationType: {$gt: 0}}]}]}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression =
        change_stream_rewrite::rewriteFilterForFields(getExpCtx(),
                                                      statusWithMatchExpression.getValue().get(),
                                                      {"clusterTime", "lsid", "operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: [{ts: {$type: [17]}}, {$nor: [{lsid: {$type: [16]}}]}]}"));
}

TEST_F(ChangeStreamRewriteTest, CannotInexactlyRewriteNegatedPredicate) {
    // Note that rewrite of {operationType: {$gt: ...}} is currently not supported. The rewrite must
    // discard the _entire_ $and, because it is the child of $nor, and the rewrite cannot use
    // inexact rewrites of any children of $not or $nor.
    auto spec = fromjson(
        "{$nor: [{clusterTime: {$type: [17]}}, "
        "{$and: [{lsid: {$type: [16]}}, {operationType: {$gt: 0}}]}]}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression =
        change_stream_rewrite::rewriteFilterForFields(getExpCtx(),
                                                      statusWithMatchExpression.getValue().get(),
                                                      {"operationType", "clusterTime", "lsid"});

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, fromjson("{$nor: [{ts: {$type: [17]}}]}"));
}

TEST_F(ChangeStreamRewriteTest, DoesNotRewriteUnrequestedField) {
    // This 'operationType' predicate could be rewritten but will be discarded in this test because
    // the 'operationType' field is not requested in the call to the 'rewriteFilterForFields()'
    // function.
    auto spec = fromjson("{operationType: 'insert', clusterTime: {$type: [17]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"clusterTime", "lsid"});

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, fromjson("{$and: [{ts: {$type: [17]}}]}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteExprWhenAllFieldsAreRenameable) {
    auto spec = fromjson(
        "{$expr: {$and: [{$eq: [{$tsSecond: '$clusterTime'}, 0]}, {$isNumber: '$lsid'}]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"clusterTime", "lsid"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$expr: {$and: [{$eq: [{$tsSecond: ['$ts']}, {$const: 0}]}, "
                               "{$isNumber: ['$lsid']}]}}"));
}

TEST_F(ChangeStreamRewriteTest, CanInexactlyRewriteExprAndWithUnrewritableChild) {
    // Note that rewrite of "$$ROOT" without a path is unsupported.
    auto spec = fromjson("{$expr: {$and: [{$isNumber: '$lsid'}, {$isArray: '$$ROOT'}]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"lsid"});

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, fromjson("{$expr: {$and: [{$isNumber: ['$lsid']}]}}"));
}

TEST_F(ChangeStreamRewriteTest, CanInexactlyRewriteExprAndWithUnknownField) {
    // $expr cannot be split into individual expressions based on dependency analysis, but because
    // we rewrite the entire tree and discard expressions which we cannot rewrite or which the user
    // did not request, we are able to produce a correct output expression even when unknown fields
    // exist.
    auto spec = fromjson("{$expr: {$and: [{$isNumber: '$lsid'}, {$isArray: '$unknown'}]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"lsid"});

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, fromjson("{$expr: {$and: [{$isNumber: ['$lsid']}]}}"));
}

TEST_F(ChangeStreamRewriteTest, CannotRewriteExprOrWithUnrewritableChild) {
    // Note that rewrite of "$$ROOT" without a path is unsupported.
    auto spec = fromjson(
        "{$expr: {$and: [{$isNumber: '$clusterTime'}, "
        "{$or: [{$isNumber: '$lsid'}, {$isArray: '$$ROOT'}]}]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"clusterTime", "lsid"});

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, fromjson("{$expr: {$and: [{$isNumber: ['$ts']}]}}"));
}

TEST_F(ChangeStreamRewriteTest, CanInexactlyRewriteExprNorWithUnrewritableChild) {
    // Note that rewrite of "$$ROOT" without a path is unsupported.
    auto spec = fromjson(
        "{$expr: {$and: [{$isNumber: '$clusterTime'},"
        "{$not: {$or: [{$isNumber: '$lsid'}, {$isArray: '$$ROOT'}]}}]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"clusterTime", "lsid"});

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        fromjson(
            "{$expr: {$and: [{$isNumber: ['$ts']}, {$not: [{$or: [{$isNumber: ['$lsid']}]}]}]}}"));
}

TEST_F(ChangeStreamRewriteTest, CanInexactlyRewriteExprNorWithUnknownField) {
    // Note that rewrite of "$$ROOT" without a path is unsupported.
    auto spec = fromjson(
        "{$expr: {$and: [{$isNumber: '$clusterTime'},"
        "{$not: {$or: [{$isNumber: '$lsid'}, {$isArray: '$unknown'}]}}]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"clusterTime", "lsid"});

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        fromjson(
            "{$expr: {$and: [{$isNumber: ['$ts']}, {$not: [{$or: [{$isNumber: ['$lsid']}]}]}]}}"));
}

TEST_F(ChangeStreamRewriteTest, CannotInexactlyRewriteNegatedExprPredicate) {
    // Note that rewrite of "$$ROOT" without a path is unsupported.
    auto spec = fromjson("{$expr: {$not: {$and: [{$isNumber: '$lsid'}, {$isArray: '$$ROOT'}]}}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"lsid"});

    ASSERT(!rewrittenMatchExpression);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteExprLet) {
    auto spec = fromjson("{$expr: {$let: {vars: {v: '$clusterTime'}, in: {$isNumber: '$$v'}}}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"clusterTime", "lsid"});

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$expr: {$let: {vars: {v: '$ts'}, in: {$isNumber: ['$$v']}}}}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteExprFieldRefWithCurrentPrefix) {
    auto spec = fromjson("{$expr: {$isNumber: '$$CURRENT.clusterTime'}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"clusterTime"});

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, fromjson("{$expr: {$isNumber: ['$ts']}}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteExprFieldRefWithRootPrefix) {
    auto spec = fromjson("{$expr: {$isNumber: '$$ROOT.clusterTime'}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"clusterTime"});

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, fromjson("{$expr: {$isNumber: ['$ts']}}"));
}

TEST_F(ChangeStreamRewriteTest, CannotRewriteExprFieldRefWithReassignedCurrentPrefix) {
    auto spec =
        fromjson("{$expr: {$let: {vars: {CURRENT: '$operationType'}, in: {$isNumber: '$lsid'}}}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"operationType", "lsid"});

    ASSERT(!rewrittenMatchExpression);
}

TEST_F(ChangeStreamRewriteTest, DoesNotRewriteUnrequestedFieldInExpr) {
    // The 'operationType' predicate could be rewritten but will be discarded in this test because
    // the 'operationType' field is not requested in the call to the 'rewriteFilterForFields()'
    // function.
    auto spec = fromjson(
        "{$expr: {$and: [{$eq: ['$operationType', 'insert']}, {$isNumber: '$clusterTime'}]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"clusterTime", "lsid"});

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, fromjson("{$expr: {$and: [{$isNumber: ['$ts']}]}}"));
}

//
// 'operationType' rewrites
//
TEST_F(ChangeStreamRewriteTest, CanRewriteEqPredicateOnOperationTypeWithInvalidOperandType) {
    auto statusWithMatchExpression =
        MatchExpressionParser::parse(BSON("operationType" << 1), getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, fromjson("{$alwaysFalse: 1}"));
}

TEST_F(ChangeStreamRewriteTest, CannotRewriteEqPredicateOnOperationTypeWithUnknownOpType) {
    auto statusWithMatchExpression = MatchExpressionParser::parse(BSON("operationType"
                                                                       << "nonExisting"),
                                                                  getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"operationType"});
    ASSERT_FALSE(rewrittenMatchExpression);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteEqNullPredicateOnOperationType) {
    auto statusWithMatchExpression =
        MatchExpressionParser::parse(BSON("operationType" << BSONNULL), getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, fromjson("{$alwaysFalse: 1}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteEqPredicateOnOperationTypeInsert) {
    auto spec = fromjson("{operationType: 'insert'}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, fromjson("{op: {$eq: 'i'}}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteEqPredicateOnOperationTypeReplace) {
    auto spec = fromjson("{operationType: 'replace'}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: [{op: {$eq: 'u'}}, {'o._id': {$exists: true}}]}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteEqPredicateOnOperationTypeDrop) {
    auto spec = fromjson("{operationType: 'drop'}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: [{op: {$eq: 'c'}}, {'o.drop': {$exists: true}}]}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteEqPredicateOnOperationTypeRename) {
    auto spec = fromjson("{operationType: 'rename'}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        fromjson("{$and: [{op: {$eq: 'c'}}, {'o.renameCollection': {$exists: true}}]}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteEqPredicateOnOperationTypeDropDatabase) {
    auto spec = fromjson("{operationType: 'dropDatabase'}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: [{op: {$eq: 'c'}}, {'o.dropDatabase': {$exists: true}}]}"));
}

TEST_F(ChangeStreamRewriteTest, CannotRewriteInequalityPredicateOnOperationType) {
    auto spec = fromjson("{operationType: {$gt: 'insert'}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"operationType"});
    ASSERT_FALSE(rewrittenMatchExpression);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteEqPredicateOnOperationTypeSubField) {
    auto spec = fromjson("{'operationType.subField': 'subOp'}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, fromjson("{$alwaysFalse: 1}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteEqNullPredicateOnOperationTypeSubField) {
    auto spec = fromjson("{'operationType.subField': {$eq: null}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, fromjson("{$alwaysTrue: 1}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteInPredicateOnOperationType) {
    auto expr = BSON("operationType" << BSON("$in" << BSON_ARRAY("drop"
                                                                 << "insert")));
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      BSON(OR(fromjson("{$and: [{op: {$eq: 'c'}}, {'o.drop': {$exists: true}}]}"),
                              fromjson("{op: {$eq: 'i'}}"))));
}

TEST_F(ChangeStreamRewriteTest, CannotRewriteInPredicateWithRegexOnOperationType) {
    auto expr = BSON("operationType" << BSON("$in" << BSON_ARRAY("/^in*sert")));
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"operationType"});
    ASSERT_FALSE(rewrittenMatchExpression);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteInPredicateOnOperationTypeWithInvalidOperandType) {
    auto expr = BSON("operationType" << BSON("$in" << BSON_ARRAY(1)));
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, BSON(OR(fromjson("{$alwaysFalse: 1}"))));
}

TEST_F(ChangeStreamRewriteTest,
       CanRewriteInPredicateOnOperationTypeWithInvalidAndValidOperandTypes) {
    auto expr = BSON("operationType" << BSON("$in" << BSON_ARRAY(1 << "insert")));
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      BSON(OR(fromjson("{$alwaysFalse: 1}"), fromjson("{op: {$eq: 'i'}}"))));
}

TEST_F(ChangeStreamRewriteTest, CannotRewriteInPredicateOnOperationTypeWithUnknownOpType) {
    auto expr = BSON("operationType" << BSON("$in" << BSON_ARRAY("unknown"
                                                                 << "insert")));
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"operationType"});
    ASSERT_FALSE(rewrittenMatchExpression);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteEmptyInPredicateOnOperationType) {
    auto spec = fromjson("{operationType: {$in: []}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, fromjson("{$alwaysFalse: 1}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNinPredicateOnOperationType) {
    auto expr = BSON("operationType" << BSON("$nin" << BSON_ARRAY("drop"
                                                                  << "insert")));
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"operationType"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(NOR(BSON(OR(fromjson("{$and: [{op: {$eq: 'c'}}, {'o.drop': {$exists: true}}]}"),
                         fromjson("{op: {$eq: 'i'}}"))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteExprWithOperationType) {
    auto spec = fromjson("{$expr: {$eq: ['$operationType', 'insert']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"operationType"});

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();

    const string expectedRewrite = R"(
{
    $expr: {
        $eq: [
            {
                $switch: {
                    branches: [
                        {case: {$eq: ['$op', {$const: 'i'}]}, then: {$const: 'insert'}},
                        {
                            case: {
                                $and:
                                    [{$eq: ['$op', {$const: 'u'}]}, {$eq: ['$o._id', '$$REMOVE']}]
                            },
                            then: {$const: 'update'}
                        },
                        {
                            case: {
                                $and:
                                    [{$eq: ['$op', {$const: 'u'}]}, {$ne: ['$o._id', '$$REMOVE']}]
                            },
                            then: {$const: 'replace'}
                        },
                        {case: {$eq: ['$op', {$const: 'd'}]}, then: {$const: 'delete'}},
                        {case: {$ne: ['$op', {$const: 'c'}]}, then: '$$REMOVE'},
                        {case: {$ne: ['$o.drop', '$$REMOVE']}, then: {$const: 'drop'}},
                        {
                            case: {$ne: ['$o.dropDatabase', '$$REMOVE']},
                            then: {$const: 'dropDatabase'}
                        },
                        {case: {$ne: ['$o.renameCollection', '$$REMOVE']}, then: {$const: 'rename'}}
                    ],
                    default: '$$REMOVE'
                }
            },
            {$const: 'insert'}
        ]
    }
})";
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, fromjson(expectedRewrite));
}

//
// 'documentKey' rewrites
//
TEST_F(ChangeStreamRewriteTest, CanRewriteEqPredicateOnFieldDocumentKey) {
    auto spec = fromjson("{documentKey: {_id: 'bar', foo: 'baz'}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"documentKey"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$or: ["
                               "  {$and: ["
                               "    {op: {$eq: 'u'}},"
                               "    {o2: {$eq: {_id: 'bar', foo: 'baz'}}}"
                               "  ]},"
                               "  {$and: ["
                               "    {op: {$eq: 'd'}},"
                               "    {o: {$eq: {_id: 'bar', foo: 'baz'}}}"
                               "  ]},"
                               "  {$and: ["
                               "    {op: {$eq: 'i'}},"
                               "    {$and: ["
                               "      {'o._id': {$eq: 'bar'}},"
                               "      {'o.foo': {$eq: 'baz'}}"
                               "    ]}"
                               "  ]}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteEqNullPredicateOnFieldDocumentKey) {
    auto spec = fromjson("{documentKey: {$eq: null}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"documentKey"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$or: ["
                               "  {$nor: ["
                               "    {op: {$eq: 'i'}},"
                               "    {op: {$eq: 'u'}},"
                               "    {op: {$eq: 'd'}}"
                               "  ]},"
                               "  {$and: ["
                               "    {op: {$eq: 'u'}},"
                               "    {o2: {$eq: null}}"
                               "  ]},"
                               "  {$and: ["
                               "    {op: {$eq: 'd'}},"
                               "    {o: {$eq: null}}"
                               "  ]},"
                               "  {$and: ["
                               "    {op: {$eq: 'i'}},"
                               "    {$alwaysFalse: 1}"
                               "  ]}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteInPredicateOnFieldDocumentKey) {
    auto spec = fromjson("{documentKey: {$in: [{}, {_id: 'bar'}, {_id: 'bar', foo: 'baz'}]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"documentKey"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$or: ["
                               "  {$and: ["
                               "    {op: {$eq: 'u'}},"
                               "    {o2: {$in: [{}, {_id: 'bar'}, {_id: 'bar', foo: 'baz'}]}}"
                               "  ]},"
                               "  {$and: ["
                               "    {op: {$eq: 'd'}},"
                               "    {o: {$in: [{}, {_id: 'bar'}, {_id: 'bar', foo: 'baz'}]}}"
                               "  ]},"
                               "  {$and: ["
                               "    {op: {$eq: 'i'}},"
                               "    {$or: ["
                               "      {$alwaysFalse: 1},"
                               "      {$and: ["
                               "        {'o._id': {$eq: 'bar'}}"
                               "      ]},"
                               "      {$and: ["
                               "        {'o._id': {$eq: 'bar'}},"
                               "        {'o.foo': {$eq: 'baz'}}"
                               "      ]}"
                               "    ]}"
                               "  ]}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest, CannotInexactlyRewriteExprPredicateOnFieldDocumentKey) {
    auto spec = fromjson("{$expr: {$or: ['$documentKey']}}");

    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"documentKey"});

    // $expr predicates that refer to the full 'documentKey' field (and not a subfield thereof)
    // cannot be rewritten (exactly or inexactly).
    ASSERT(rewrittenMatchExpression == nullptr);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteArbitraryPredicateOnFieldDocumentKeyId) {
    auto spec = fromjson("{'documentKey._id': {$lt: 'bar'}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"documentKey"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$or: ["
                               "  {$and: ["
                               "    {op: {$eq: 'u'}},"
                               "    {'o2._id': {$lt: 'bar'}}"
                               "  ]},"
                               "  {$and: ["
                               "    {op: {$eq: 'd'}},"
                               "    {'o._id': {$lt: 'bar'}}"
                               "  ]},"
                               "  {$and: ["
                               "    {op: {$eq: 'i'}},"
                               "    {'o._id': {$lt: 'bar'}}"
                               "  ]}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteEqNullPredicateOnFieldDocumentKeyId) {
    auto spec = fromjson("{'documentKey._id': {$eq: null}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"documentKey"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$or: ["
                               "  {$nor: ["
                               "    {op: {$eq: 'i'}},"
                               "    {op: {$eq: 'u'}},"
                               "    {op: {$eq: 'd'}}"
                               "  ]},"
                               "  {$and: ["
                               "    {op: {$eq: 'u'}},"
                               "    {'o2._id': {$eq: null}}"
                               "  ]},"
                               "  {$and: ["
                               "    {op: {$eq: 'd'}},"
                               "    {'o._id': {$eq: null}}"
                               "  ]},"
                               "  {$and: ["
                               "    {op: {$eq: 'i'}},"
                               "    {'o._id': {$eq: null}}"
                               "  ]}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest, CanExactlyRewriteExprPredicateOnFieldDocumentKeyId) {
    auto spec = fromjson("{$expr: {$lt: ['$documentKey._id', 'bar']}}");

    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"documentKey"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        fromjson("{$expr:"
                 "  {$lt: ["
                 "    {$switch: {"
                 "      branches: ["
                 "        {case: {$in: ['$op', [{$const: 'i'}, {$const: 'd'}]]}, then: '$o._id'},"
                 "        {case: {$eq: ['$op', {$const: 'u'}]}, then: '$o2._id'}"
                 "      ],"
                 "      default: '$$REMOVE'"
                 "    }},"
                 "    {$const: 'bar'}"
                 "  ]}"
                 "}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteArbitraryPredicateOnFieldDocumentKeyFoo) {
    auto spec = fromjson("{'documentKey.foo': {$gt: 'bar'}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"documentKey"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$or: ["
                               "  {$and: ["
                               "    {op: {$eq: 'u'}},"
                               "    {'o2.foo': {$gt: 'bar'}}"
                               "  ]},"
                               "  {$and: ["
                               "    {op: {$eq: 'd'}},"
                               "    {'o.foo': {$gt: 'bar'}}"
                               "  ]},"
                               "  {$and: ["
                               "    {op: {$eq: 'i'}},"
                               "    {'o.foo': {$gt: 'bar'}}"
                               "  ]}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteEqNullPredicateOnFieldDocumentKeyFoo) {
    auto spec = fromjson("{'documentKey.foo': {$eq: null}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"documentKey"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$or: ["
                               "  {$nor: ["
                               "    {op: {$eq: 'i'}},"
                               "    {op: {$eq: 'u'}},"
                               "    {op: {$eq: 'd'}}"
                               "  ]},"
                               "  {$and: ["
                               "    {op: {$eq: 'u'}},"
                               "    {'o2.foo': {$eq: null}}"
                               "  ]},"
                               "  {$and: ["
                               "    {op: {$eq: 'd'}},"
                               "    {'o.foo': {$eq: null}}"
                               "  ]},"
                               "  {op: {$eq: 'i'}}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest, CannotExactlyRewritePredicateOnFieldDocumentKey) {
    auto spec = fromjson("{documentKey: {$not: {$eq: {_id: 'bar'}}}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"documentKey"});

    // Insert events do not have a specific documentKey field in the oplog, so filters on the full
    // documentKey field cannot be exactly rewritten.
    ASSERT(rewrittenMatchExpression == nullptr);
}

TEST_F(ChangeStreamRewriteTest, CanExactlyRewritePredicateOnFieldDocumentKeyId) {
    auto spec = fromjson("{'documentKey._id': {$not: {$eq: 'bar'}}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"documentKey"});
    ASSERT(rewrittenMatchExpression);

    // Every documentKey must have an _id field, so a predicate on this field can always be exactly
    // rewritten.
    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$nor: ["
                               "  {$or: ["
                               "    {$and: ["
                               "      {op: {$eq: 'u'}},"
                               "      {'o2._id': {$eq: 'bar'}}"
                               "    ]},"
                               "    {$and: ["
                               "      {op: {$eq: 'd'}},"
                               "      {'o._id': {$eq: 'bar'}}"
                               "    ]},"
                               "    {$and: ["
                               "      {op: {$eq: 'i'}},"
                               "      {'o._id': {$eq: 'bar'}}"
                               "    ]}"
                               "  ]}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest, CannotExactlyRewritePredicateOnFieldDocumentKeyFoo) {
    auto spec = fromjson("{'documentKey.foo': {$not: {$eq: 'bar'}}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"documentKey"});

    // We cannot be sure that the path for this predicate is actually a valid field in the
    // documentKey. Therefore, we cannot exactly rewrite a predicate on this field.
    ASSERT(rewrittenMatchExpression == nullptr);
}

TEST_F(ChangeStreamRewriteTest, CanInexactlyRewriteExprPredicateOnFieldDocumentKeyFoo) {
    auto spec = fromjson("{$expr: {$or: ['$documentKey.foo']}}");

    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"documentKey"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        fromjson("{$expr:"
                 "  {$or: ["
                 "    {$switch: {"
                 "      branches: ["
                 "        {case: {$in: ['$op', [{$const: 'i'}, {$const: 'd'}]]}, then: '$o.foo'},"
                 "        {case: {$eq: ['$op', {$const: 'u'}]}, then: '$o2.foo'}"
                 "      ],"
                 "      default: '$$REMOVE'"
                 "    }}"
                 "  ]}"
                 "}"));
}

TEST_F(ChangeStreamRewriteTest, CannotExactlyRewriteExprPredicateOnFieldDocumentKeyFoo) {
    auto spec = fromjson("{$expr: {$lt: ['$documentKey.foo', 'bar']}}");

    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"documentKey"});

    // We cannot be sure that the field path is actually a valid field in the documentKey.
    // Therefore, we cannot exactly rewrite this expression.
    ASSERT(rewrittenMatchExpression == nullptr);
}

//
// 'fullDocument' rewrites
//
TEST_F(ChangeStreamRewriteTest, CanRewriteArbitraryPredicateOnFieldFullDocumentFoo) {
    auto spec = fromjson("{'fullDocument.foo': {$lt: 'bar'}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"fullDocument"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$or: ["
                               "  {$and: ["
                               "    {op: {$eq: 'u'}},"
                               "    {'o._id': {$not: {$exists: true}}}"
                               "  ]},"
                               "  {$and: ["
                               "    {$or: ["
                               "      {$and: ["
                               "        {op: {$eq: 'u'}},"
                               "        {'o._id': {$exists: true}}"
                               "      ]},"
                               "      {op: {$eq: 'i'}}"
                               "    ]},"
                               "    {'o.foo': {$lt: 'bar'}}"
                               "  ]}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNullComparisonPredicateOnFieldFullDocumentFoo) {
    auto spec = fromjson("{'fullDocument.foo': {$eq: null}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"fullDocument"});
    ASSERT(rewrittenMatchExpression);

    // Note that the filter below includes a predicate on delete and non-CRUD events. These are only
    // present when the user's predicate matches a non-existent field. This is because these change
    // events never have a 'fullDocument' field, and so we either match all such events in the oplog
    // or none of them, depending on how the predicate evaluates against a missing field.
    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$or: ["
                               "  {$and: ["
                               "    {op: {$eq: 'u'}},"
                               "    {'o._id': {$not: {$exists: true}}}"
                               "  ]},"
                               "  {$and: ["
                               "    {$or: ["
                               "      {$and: ["
                               "        {op: {$eq: 'u'}},"
                               "        {'o._id': {$exists: true}}"
                               "      ]},"
                               "      {op: {$eq: 'i'}}"
                               "    ]},"
                               "    {'o.foo': {$eq: null}}"
                               "  ]},"
                               "  {op: {$eq: 'd'}},"
                               "  {$nor: [{op: {$eq: 'i'}}, {op: {$eq: 'u'}}, {op: {$eq: 'd'}}]}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest, CannotExactlyRewritePredicateOnFieldFullDocumentFoo) {
    auto spec = fromjson("{'fullDocument.foo': {$not: {$eq: 'bar'}}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(spec, getExpCtx());
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"fullDocument"});

    // Because the 'fullDocument' field can be populated later in the pipeline for update events
    // (via the '{fullDocument: "updateLookup"}' option), it's impractical to try to generate a
    // rewritten predicate that matches exactly.
    ASSERT(rewrittenMatchExpression == nullptr);
}

//
// 'ns' rewrites
//
TEST_F(ChangeStreamRewriteTest, CanRewriteFullNamespaceObject) {
    auto expCtx = getExpCtx();
    auto statusWithMatchExpression = MatchExpressionParser::parse(
        BSON("ns" << BSON("db" << expCtx->ns.db() << "coll" << expCtx->ns.coll())), expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    const std::string ns = expCtx->ns.db().toString() + "." + expCtx->ns.coll().toString();
    const std::string cmdNs = expCtx->ns.db().toString() + ".$cmd";

    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"), BSON("ns" << BSON("$eq" << ns)))),
                BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                         BSON(OR(BSON("o.renameCollection" << BSON("$eq" << ns)),
                                 BSON(AND(BSON("ns" << BSON("$eq" << cmdNs)),
                                          BSON("o.drop" << BSON("$eq" << expCtx->ns.coll())))),
                                 BSON(AND(fromjson("{$alwaysFalse: 1}"),
                                          fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceObjectWithSwappedField) {
    auto expCtx = getExpCtx();
    auto statusWithMatchExpression = MatchExpressionParser::parse(
        BSON("ns" << BSON("coll" << expCtx->ns.coll() << "db" << expCtx->ns.db())), expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"), fromjson("{$alwaysFalse: 1 }"))),
                BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                         BSON(OR(fromjson("{$alwaysFalse: 1 }"),
                                 fromjson("{$alwaysFalse: 1 }"),
                                 BSON(AND(fromjson("{$alwaysFalse: 1}"),
                                          fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceObjectWithOnlyDbField) {
    auto expCtx = getExpCtx();
    auto statusWithMatchExpression =
        MatchExpressionParser::parse(BSON("ns" << BSON("db" << expCtx->ns.db())), expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"ns"});
    ASSERT(rewrittenMatchExpression);

    const std::string cmdNs = expCtx->ns.db().toString() + ".$cmd";

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"), fromjson("{$alwaysFalse: 1}"))),
                BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                         BSON(OR(fromjson("{$alwaysFalse: 1 }"),
                                 fromjson("{$alwaysFalse: 1 }"),
                                 BSON(AND(BSON(AND(BSON("ns" << BSON("$eq" << cmdNs)))),
                                          fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceObjectWithOnlyCollectionField) {
    auto expCtx = getExpCtx();
    auto statusWithMatchExpression =
        MatchExpressionParser::parse(BSON("ns" << BSON("coll" << expCtx->ns.coll())), expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"), fromjson("{$alwaysFalse: 1 }"))),
                BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                         BSON(OR(fromjson("{$alwaysFalse: 1 }"),
                                 fromjson("{$alwaysFalse: 1 }"),
                                 BSON(AND(fromjson("{$alwaysFalse: 1}"),
                                          fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceObjectWithInvalidDbField) {
    auto expCtx = getExpCtx();
    auto statusWithMatchExpression = MatchExpressionParser::parse(
        BSON("ns" << BSON("db" << 1 << "coll" << expCtx->ns.coll())), expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"), fromjson("{$alwaysFalse: 1 }"))),
                BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                         BSON(OR(fromjson("{$alwaysFalse: 1 }"),
                                 fromjson("{$alwaysFalse: 1 }"),
                                 BSON(AND(fromjson("{$alwaysFalse: 1}"),
                                          fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceObjectWithInvalidCollField) {
    auto expCtx = getExpCtx();
    auto statusWithMatchExpression = MatchExpressionParser::parse(
        BSON("ns" << BSON("db" << expCtx->ns.db() << "coll" << 1)), expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"), fromjson("{$alwaysFalse: 1 }"))),
                BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                         BSON(OR(fromjson("{$alwaysFalse: 1 }"),
                                 fromjson("{$alwaysFalse: 1 }"),
                                 BSON(AND(fromjson("{$alwaysFalse: 1}"),
                                          fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceObjectWithExtraField) {
    auto expCtx = getExpCtx();
    auto statusWithMatchExpression = MatchExpressionParser::parse(
        fromjson("{ns: {db: 'db', coll: 'coll', extra: 'extra'}}"), expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"), fromjson("{$alwaysFalse: 1 }"))),
                BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                         BSON(OR(fromjson("{$alwaysFalse: 1 }"),
                                 fromjson("{$alwaysFalse: 1 }"),
                                 BSON(AND(fromjson("{$alwaysFalse: 1}"),
                                          fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithStringDbFieldPath) {
    auto expCtx = getExpCtx();
    auto statusWithMatchExpression =
        MatchExpressionParser::parse(BSON("ns.db" << expCtx->ns.db()), expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    const std::string regexNs = "^" + expCtx->ns.db().toString() + "\\." +
        DocumentSourceChangeStream::kRegexAllCollections.toString();
    const std::string cmdNs = expCtx->ns.db().toString() + ".$cmd";

    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"),
                         BSON("ns" << BSON("$regex" << regexNs)))),
                BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                         BSON(OR(BSON("o.renameCollection" << BSON("$regex" << regexNs)),
                                 BSON("ns" << BSON("$eq" << cmdNs)),
                                 BSON(AND(BSON("ns" << BSON("$eq" << cmdNs)),
                                          fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithCollectionFieldPath) {
    auto expCtx = getExpCtx();
    auto statusWithMatchExpression =
        MatchExpressionParser::parse(BSON("ns.coll" << expCtx->ns.coll()), expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    const std::string regexNs = DocumentSourceChangeStream::kRegexAllDBs.toString() + "\\." +
        expCtx->ns.coll().toString() + "$";

    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"),
                         BSON("ns" << BSON("$regex" << regexNs)))),
                BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                         BSON(OR(BSON("o.renameCollection" << BSON("$regex" << regexNs)),
                                 BSON("o.drop" << BSON("$eq" << expCtx->ns.coll())),
                                 BSON(AND(fromjson("{$alwaysFalse: 1}"),
                                          fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithRegexDbFieldPath) {
    auto expCtx = getExpCtx();
    auto statusWithMatchExpression =
        MatchExpressionParser::parse(BSON("ns.db" << BSONRegEx(R"(^unit.*$)")), expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(
            BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"),
                     fromjson(getNsDbRegexMatchExpr("$ns", R"(^unit.*$)")))),
            BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                     BSON(OR(fromjson(getNsDbRegexMatchExpr("$o.renameCollection", R"(^unit.*$)")),
                             fromjson(getNsDbRegexMatchExpr("$ns", R"(^unit.*$)")),
                             BSON(AND(fromjson(getNsDbRegexMatchExpr("$ns", R"(^unit.*$)")),
                                      fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithRegexCollectionFieldPath) {
    auto expCtx = getExpCtx();
    auto statusWithMatchExpression =
        MatchExpressionParser::parse(BSON("ns.coll" << BSONRegEx(R"(^pipeline.*$)")), expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"),
                         fromjson(getNsCollRegexMatchExpr("$ns", R"(^pipeline.*$)")))),
                BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                         BSON(OR(fromjson(getNsCollRegexMatchExpr("$o.renameCollection",
                                                                  R"(^pipeline.*$)")),
                                 fromjson(getNsCollRegexMatchExpr("$o.drop", R"(^pipeline.*$)")),
                                 BSON(AND(fromjson("{ $alwaysFalse: 1 }"),
                                          fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CannotRewriteNamespaceWithInvalidDbFieldPath) {
    auto expCtx = getExpCtx();
    auto statusWithMatchExpression = MatchExpressionParser::parse(BSON("ns.db" << 1), expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"ns"});
    ASSERT_FALSE(rewrittenMatchExpression);
}

TEST_F(ChangeStreamRewriteTest, CannotRewriteNamespaceWithInvalidCollectionFieldPath) {
    auto expCtx = getExpCtx();
    auto statusWithMatchExpression = MatchExpressionParser::parse(BSON("ns.coll" << 1), expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"ns"});
    ASSERT_FALSE(rewrittenMatchExpression);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithExtraDbFieldPath) {
    auto expCtx = getExpCtx();
    auto statusWithMatchExpression = MatchExpressionParser::parse(BSON("ns.db.subField"
                                                                       << "subDb"),
                                                                  expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"), fromjson("{$alwaysFalse: 1 }"))),
                BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                         BSON(OR(fromjson("{$alwaysFalse: 1 }"),
                                 fromjson("{$alwaysFalse: 1 }"),
                                 BSON(AND(fromjson("{$alwaysFalse: 1 }"),
                                          fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithExtraCollectionFieldPath) {
    auto expCtx = getExpCtx();
    auto statusWithMatchExpression = MatchExpressionParser::parse(BSON("ns.coll.subField"
                                                                       << "subColl"),
                                                                  expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"), fromjson("{$alwaysFalse: 1 }"))),
                BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                         BSON(OR(fromjson("{$alwaysFalse: 1 }"),
                                 fromjson("{$alwaysFalse: 1 }"),
                                 BSON(AND(fromjson("{$alwaysFalse: 1 }"),
                                          fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithInvalidFieldPath) {
    auto expCtx = getExpCtx();
    auto statusWithMatchExpression = MatchExpressionParser::parse(BSON("ns.unknown"
                                                                       << "test"),
                                                                  expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"), fromjson("{$alwaysFalse: 1 }"))),
                BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                         BSON(OR(fromjson("{$alwaysFalse: 1 }"),
                                 fromjson("{$alwaysFalse: 1 }"),
                                 BSON(AND(fromjson("{$alwaysFalse: 1 }"),
                                          fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithInExpressionOnDb) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'ns.db': {$in: ['test', 'news']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"ns"});
    ASSERT(rewrittenMatchExpression);

    const std::string firstRegexNs = std::string("^") + "news" + "\\." +
        DocumentSourceChangeStream::kRegexAllCollections.toString();
    const std::string secondRegexNs = std::string("^") + "test" + "\\." +
        DocumentSourceChangeStream::kRegexAllCollections.toString();
    const std::string firstCmdNs = "news.$cmd";
    const std::string secondCmdNs = "test.$cmd";

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"),
                         BSON(OR(BSON("ns" << BSON("$regex" << firstRegexNs)),
                                 BSON("ns" << BSON("$regex" << secondRegexNs)))))),
                BSON(AND(
                    fromjson("{op: {$eq: 'c'}}"),
                    BSON(OR(BSON(OR(BSON("o.renameCollection" << BSON("$regex" << firstRegexNs)),
                                    BSON("o.renameCollection" << BSON("$regex" << secondRegexNs)))),
                            BSON(OR(BSON("ns" << BSON("$eq" << firstCmdNs)),
                                    BSON("ns" << BSON("$eq" << secondCmdNs)))),
                            BSON(AND(BSON(OR(BSON("ns" << BSON("$eq" << firstCmdNs)),
                                             BSON("ns" << BSON("$eq" << secondCmdNs)))),
                                     fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithNinExpressionOnDb) {
    auto expCtx = getExpCtx();
    auto expr = BSON("ns.db" << BSON("$nin" << BSON_ARRAY("test"
                                                          << "news")));
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"ns"});
    ASSERT(rewrittenMatchExpression);

    const std::string firstRegexNs = std::string("^") + "test" + "\\." +
        DocumentSourceChangeStream::kRegexAllCollections.toString();
    const std::string secondRegexNs = std::string("^") + "news" + "\\." +
        DocumentSourceChangeStream::kRegexAllCollections.toString();
    const std::string firstCmdNs = "test.$cmd";
    const std::string secondCmdNs = "news.$cmd";

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(NOR(BSON(OR(
            BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"),
                     BSON(OR(BSON("ns" << BSON("$regex" << secondRegexNs)),
                             BSON("ns" << BSON("$regex" << firstRegexNs)))))),
            BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                     BSON(OR(BSON(OR(BSON("o.renameCollection" << BSON("$regex" << secondRegexNs)),
                                     BSON("o.renameCollection" << BSON("$regex" << firstRegexNs)))),
                             BSON(OR(BSON("ns" << BSON("$eq" << secondCmdNs)),
                                     BSON("ns" << BSON("$eq" << firstCmdNs)))),
                             BSON(AND(BSON(OR(BSON("ns" << BSON("$eq" << secondCmdNs)),
                                              BSON("ns" << BSON("$eq" << firstCmdNs)))),
                                      fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithInExpressionOnCollection) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'ns.coll': {$in: ['test', 'news']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"ns"});
    ASSERT(rewrittenMatchExpression);

    const std::string firstRegexNs =
        DocumentSourceChangeStream::kRegexAllDBs.toString() + "\\." + "news" + "$";
    const std::string secondRegexNs =
        DocumentSourceChangeStream::kRegexAllDBs.toString() + "\\." + "test" + "$";

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"),
                         BSON(OR(BSON("ns" << BSON("$regex" << firstRegexNs)),
                                 BSON("ns" << BSON("$regex" << secondRegexNs)))))),
                BSON(AND(
                    fromjson("{op: {$eq: 'c'}}"),
                    BSON(OR(BSON(OR(BSON("o.renameCollection" << BSON("$regex" << firstRegexNs)),
                                    BSON("o.renameCollection" << BSON("$regex" << secondRegexNs)))),
                            BSON(OR(BSON("o.drop" << BSON("$eq"
                                                          << "news")),
                                    BSON("o.drop" << BSON("$eq"
                                                          << "test")))),
                            BSON(AND(BSON(OR(fromjson("{$alwaysFalse: 1}"),
                                             fromjson("{$alwaysFalse: 1}"))),
                                     fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithNinExpressionOnCollection) {
    auto expCtx = getExpCtx();
    auto expr = BSON("ns.coll" << BSON("$nin" << BSON_ARRAY("test"
                                                            << "news")));
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"ns"});
    ASSERT(rewrittenMatchExpression);

    const std::string firstRegexNs =
        DocumentSourceChangeStream::kRegexAllDBs.toString() + "\\." + "test" + "$";
    const std::string secondRegexNs =
        DocumentSourceChangeStream::kRegexAllDBs.toString() + "\\." + "news" + "$";

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(NOR(BSON(OR(
            BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"),
                     BSON(OR(BSON("ns" << BSON("$regex" << secondRegexNs)),
                             BSON("ns" << BSON("$regex" << firstRegexNs)))))),
            BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                     BSON(OR(BSON(OR(BSON("o.renameCollection" << BSON("$regex" << secondRegexNs)),
                                     BSON("o.renameCollection" << BSON("$regex" << firstRegexNs)))),
                             BSON(OR(BSON("o.drop" << BSON("$eq"
                                                           << "news")),
                                     BSON("o.drop" << BSON("$eq"
                                                           << "test")))),
                             BSON(AND(BSON(OR(fromjson("{$alwaysFalse: 1}"),
                                              fromjson("{$alwaysFalse: 1}"))),
                                      fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithInRegexExpressionOnDb) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'ns.db': {$in: [/^test.*$/, /^news$/]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(
            BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"),
                     BSON(OR(fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")),
                             fromjson(getNsDbRegexMatchExpr("$ns", R"(^news$)")))))),
            BSON(AND(
                fromjson("{op: {$eq: 'c'}}"),
                BSON(OR(
                    BSON(OR(fromjson(getNsDbRegexMatchExpr("$o.renameCollection", R"(^test.*$)")),
                            fromjson(getNsDbRegexMatchExpr("$o.renameCollection", R"(^news$)")))),
                    BSON(OR(fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")),
                            fromjson(getNsDbRegexMatchExpr("$ns", R"(^news$)")))),
                    BSON(AND(BSON(OR(fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")),
                                     fromjson(getNsDbRegexMatchExpr("$ns", R"(^news$)")))),
                             fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithNinRegexExpressionOnDb) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'ns.db': {$nin: [/^test.*$/, /^news$/]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(NOR(BSON(OR(
            BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"),
                     BSON(OR(fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")),
                             fromjson(getNsDbRegexMatchExpr("$ns", R"(^news$)")))))),
            BSON(AND(
                fromjson("{op: {$eq: 'c'}}"),
                BSON(OR(
                    BSON(OR(fromjson(getNsDbRegexMatchExpr("$o.renameCollection", R"(^test.*$)")),
                            fromjson(getNsDbRegexMatchExpr("$o.renameCollection", R"(^news$)")))),
                    BSON(OR(fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")),
                            fromjson(getNsDbRegexMatchExpr("$ns", R"(^news$)")))),
                    BSON(AND(BSON(OR(fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")),
                                     fromjson(getNsDbRegexMatchExpr("$ns", R"(^news$)")))),
                             fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithInRegexExpressionOnCollection) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'ns.coll': {$in: [/^test.*$/, /^news$/]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(
            BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"),
                     BSON(OR(fromjson(getNsCollRegexMatchExpr("$ns", R"(^test.*$)")),
                             fromjson(getNsCollRegexMatchExpr("$ns", R"(^news$)")))))),
            BSON(AND(
                fromjson("{op: {$eq: 'c'}}"),
                BSON(OR(
                    BSON(OR(fromjson(getNsCollRegexMatchExpr("$o.renameCollection", R"(^test.*$)")),
                            fromjson(getNsCollRegexMatchExpr("$o.renameCollection", R"(^news$)")))),
                    BSON(OR(fromjson(getNsCollRegexMatchExpr("$o.drop", R"(^test.*$)")),
                            fromjson(getNsCollRegexMatchExpr("$o.drop", R"(^news$)")))),
                    BSON(AND(BSON(OR(fromjson("{$alwaysFalse: 1}"), fromjson("{$alwaysFalse: 1}"))),
                             fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithNinRegexExpressionOnCollection) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'ns.coll': {$nin: [/^test.*$/, /^news$/]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(NOR(BSON(OR(
            BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"),
                     BSON(OR(fromjson(getNsCollRegexMatchExpr("$ns", R"(^test.*$)")),
                             fromjson(getNsCollRegexMatchExpr("$ns", R"(^news$)")))))),
            BSON(AND(
                fromjson("{op: {$eq: 'c'}}"),
                BSON(OR(
                    BSON(OR(fromjson(getNsCollRegexMatchExpr("$o.renameCollection", R"(^test.*$)")),
                            fromjson(getNsCollRegexMatchExpr("$o.renameCollection", R"(^news$)")))),
                    BSON(OR(fromjson(getNsCollRegexMatchExpr("$o.drop", R"(^test.*$)")),
                            fromjson(getNsCollRegexMatchExpr("$o.drop", R"(^news$)")))),
                    BSON(AND(BSON(OR(fromjson("{$alwaysFalse: 1}"), fromjson("{$alwaysFalse: 1}"))),
                             fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithInExpressionOnDbWithRegexAndString) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'ns.db': {$in: [/^test.*$/, 'news']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"ns"});
    ASSERT(rewrittenMatchExpression);

    const std::string secondRegexNs = std::string("^") + "news" + "\\." +
        DocumentSourceChangeStream::kRegexAllCollections.toString();
    const std::string secondCmdNs = "news.$cmd";

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(
            BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"),
                     BSON(OR(BSON("ns" << BSON("$regex" << secondRegexNs)),
                             fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")))))),
            BSON(AND(
                fromjson("{op: {$eq: 'c'}}"),
                BSON(OR(
                    BSON(OR(BSON("o.renameCollection" << BSON("$regex" << secondRegexNs)),
                            fromjson(getNsDbRegexMatchExpr("$o.renameCollection", R"(^test.*$)")))),
                    BSON(OR(BSON("ns" << BSON("$eq" << secondCmdNs)),
                            fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")))),
                    BSON(AND(BSON(OR(BSON("ns" << BSON("$eq" << secondCmdNs)),
                                     fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")))),
                             fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithNinExpressionOnDbWithRegexAndString) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'ns.db': {$nin: [/^test.*$/, 'news']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"ns"});
    ASSERT(rewrittenMatchExpression);


    const std::string secondRegexNs = std::string("^") + "news" + "\\." +
        DocumentSourceChangeStream::kRegexAllCollections.toString();
    const std::string secondCmdNs = "news.$cmd";

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(NOR(BSON(OR(
            BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"),
                     BSON(OR(BSON("ns" << BSON("$regex" << secondRegexNs)),
                             fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")))))),
            BSON(AND(
                fromjson("{op: {$eq: 'c'}}"),
                BSON(OR(
                    BSON(OR(BSON("o.renameCollection" << BSON("$regex" << secondRegexNs)),
                            fromjson(getNsDbRegexMatchExpr("$o.renameCollection", R"(^test.*$)")))),
                    BSON(OR(BSON("ns" << BSON("$eq" << secondCmdNs)),
                            fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")))),
                    BSON(AND(BSON(OR(BSON("ns" << BSON("$eq" << secondCmdNs)),
                                     fromjson(getNsDbRegexMatchExpr("$ns", R"(^test.*$)")))),
                             fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithInExpressionOnCollectionWithRegexAndString) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'ns.coll': {$in: [/^test.*$/, 'news']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"ns"});
    ASSERT(rewrittenMatchExpression);

    const std::string secondRegexNs =
        DocumentSourceChangeStream::kRegexAllDBs + "\\." + "news" + "$";
    const std::string secondCmdNs = "news.$cmd";

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(
            BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"),
                     BSON(OR(BSON("ns" << BSON("$regex" << secondRegexNs)),
                             fromjson(getNsCollRegexMatchExpr("$ns", R"(^test.*$)")))))),
            BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                     BSON(OR(BSON(OR(BSON("o.renameCollection" << BSON("$regex" << secondRegexNs)),
                                     fromjson(getNsCollRegexMatchExpr("$o.renameCollection",
                                                                      R"(^test.*$)")))),
                             BSON(OR(BSON("o.drop" << BSON("$eq"
                                                           << "news")),
                                     fromjson(getNsCollRegexMatchExpr("$o.drop", R"(^test.*$)")))),
                             BSON(AND(BSON(OR(fromjson("{$alwaysFalse: 1}"),
                                              fromjson("{$alwaysFalse: 1}"))),
                                      fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest,
       CanRewriteNamespaceWithNinExpressionOnCollectionWithRegexAndString) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'ns.coll': {$nin: [/^test.*$/, 'news']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"ns"});
    ASSERT(rewrittenMatchExpression);

    const std::string secondRegexNs =
        DocumentSourceChangeStream::kRegexAllDBs + "\\." + "news" + "$";
    const std::string secondCmdNs = "news.$cmd";

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(NOR(BSON(OR(
            BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"),
                     BSON(OR(BSON("ns" << BSON("$regex" << secondRegexNs)),
                             fromjson(getNsCollRegexMatchExpr("$ns", R"(^test.*$)")))))),
            BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                     BSON(OR(BSON(OR(BSON("o.renameCollection" << BSON("$regex" << secondRegexNs)),
                                     fromjson(getNsCollRegexMatchExpr("$o.renameCollection",
                                                                      R"(^test.*$)")))),
                             BSON(OR(BSON("o.drop" << BSON("$eq"
                                                           << "news")),
                                     fromjson(getNsCollRegexMatchExpr("$o.drop", R"(^test.*$)")))),
                             BSON(AND(BSON(OR(fromjson("{$alwaysFalse: 1}"),
                                              fromjson("{$alwaysFalse: 1}"))),
                                      fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))))));
}

TEST_F(ChangeStreamRewriteTest, CannotRewriteNamespaceWithInExpressionOnInvalidDb) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'ns.db': {$in: ['test', 1]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"ns"});
    ASSERT_FALSE(rewrittenMatchExpression);
}

TEST_F(ChangeStreamRewriteTest, CannotRewriteNamespaceWithNinExpressionOnInvalidDb) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'ns.db': {$nin: ['test', 1]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"ns"});
    ASSERT_FALSE(rewrittenMatchExpression);
}

TEST_F(ChangeStreamRewriteTest, CannotRewriteNamespaceWithInExpressionOnInvalidCollection) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'ns.coll': {$in: ['coll', 1]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"ns"});
    ASSERT_FALSE(rewrittenMatchExpression);
}

TEST_F(ChangeStreamRewriteTest, CannotRewriteNamespaceWithNinExpressionOnInvalidCollection) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'ns.coll': {$nin: ['coll', 1]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"ns"});
    ASSERT_FALSE(rewrittenMatchExpression);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithEmptyInExpression) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'ns.db': {$in: []}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(OR(BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"), fromjson("{$alwaysFalse: 1 }"))),
                BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                         BSON(OR(fromjson("{$alwaysFalse: 1 }"),
                                 fromjson("{$alwaysFalse: 1 }"),
                                 BSON(AND(fromjson("{$alwaysFalse: 1 }"),
                                          fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNamespaceWithEmptyNinExpression) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'ns.db': {$nin: []}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(NOR(
            BSON(OR(BSON(AND(fromjson("{op: {$not: {$eq: 'c'}}}"), fromjson("{$alwaysFalse: 1 }"))),
                    BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                             BSON(OR(fromjson("{$alwaysFalse: 1 }"),
                                     fromjson("{$alwaysFalse: 1 }"),
                                     BSON(AND(fromjson("{$alwaysFalse: 1 }"),
                                              fromjson("{'o.dropDatabase': {$eq: 1}}"))))))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNsWithExprOnFullObject) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{$expr: {$eq: ['$ns', {db: '" + expCtx->ns.db() + "', coll: '" +
                         expCtx->ns.coll() + "'}]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto case1 =
        "{case: {$in: ['$op', [{$const: 'i' }, {$const: 'u' }, {$const: 'd'}]]}, then: "
        "{$substrBytes: ['$ns', {$add: [{$strLenBytes: ['$$dbName']}, {$const: 1}]}, {$const: "
        "-1}]}}";
    auto case2 = "{case: { $ne: ['$op', {$const: 'c'}]}, then: '$$REMOVE'}";
    auto case3 = "{case: {$ne: ['$o.drop', '$$REMOVE']}, then: '$o.drop'}";
    auto case4 = "{case: {$ne: ['$o.dropDatabase', '$$REMOVE']}, then: '$$REMOVE'}";
    auto case5 =
        "{case: {$ne: ['$o.renameCollection', '$$REMOVE']}, then: {$substrBytes: "
        "['$o.renameCollection', {$add: [{$strLenBytes: ['$$dbName']}, {$const: 1}]}, {$const: "
        "-1}]}}";

    auto expectedExpr = fromjson(
        "{"
        "  $expr: {"
        "    $eq: [{"
        "      $let: {"
        "        vars: {"
        "          dbName: {$substrBytes: ["
        "            '$ns', "
        "            {$const: 0}, "
        "            {$indexOfBytes: ['$ns', {$const: '.'}]}]}}, "
        "        in: {"
        "          db: '$$dbName',"
        "          coll: {"
        "            $switch: {"
        "              branches: ["s +
        "                " + case1 + ", " + case2 + ", " + case3 + ", " + case4 + ", " + case5 +
        "              ], default: '$$REMOVE'}}}}}, "
        "      {db: {$const: 'unittests' }, coll: {$const: 'pipeline_test'}}]}"
        "}");

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, expectedExpr);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNsWithExprOnFullObjectWithOnlyDb) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{$expr: {$eq: ['$ns', {db: '" + expCtx->ns.db() + "'}]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto case1 =
        "{case: {$in: ['$op', [{$const: 'i' }, {$const: 'u' }, {$const: 'd'}]]}, then: "
        "{$substrBytes: ['$ns', {$add: [{$strLenBytes: ['$$dbName']}, {$const: 1}]}, {$const: "
        "-1}]}}";
    auto case2 = "{case: { $ne: ['$op', {$const: 'c'}]}, then: '$$REMOVE'}";
    auto case3 = "{case: {$ne: ['$o.drop', '$$REMOVE']}, then: '$o.drop'}";
    auto case4 = "{case: {$ne: ['$o.dropDatabase', '$$REMOVE']}, then: '$$REMOVE'}";
    auto case5 =
        "{case: {$ne: ['$o.renameCollection', '$$REMOVE']}, then: {$substrBytes: "
        "['$o.renameCollection', {$add: [{$strLenBytes: ['$$dbName']}, {$const: 1}]}, {$const: "
        "-1}]}}";

    auto expectedExpr = fromjson(
        "{"
        "  $expr: {"
        "    $eq: [{"
        "      $let: {"
        "        vars: {"
        "          dbName: {$substrBytes: ["
        "            '$ns', "
        "            {$const: 0}, "
        "            {$indexOfBytes: ['$ns', {$const: '.'}]}]}}, "
        "        in: {"
        "          db: '$$dbName',"
        "          coll: {"
        "            $switch: {"
        "              branches: ["s +
        "                " + case1 + ", " + case2 + ", " + case3 + ", " + case4 + ", " + case5 +
        "              ], default: '$$REMOVE'}}}}}, "
        "      {db: {$const: 'unittests' }}]}"
        "}");

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, expectedExpr);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNsWithExprOnDbFieldPath) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{$expr: {$eq: ['$ns.db', '" + expCtx->ns.coll() + "']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto expectedExpr = fromjson(
        "{"
        "  $expr: {"
        "    $eq: [{"
        "      $let: {"
        "        vars: {"
        "          dbName: {$substrBytes: ["
        "            '$ns', "
        "            {$const: 0}, "
        "            {$indexOfBytes: ['$ns', {$const: '.'}]}]}}, "
        "        in: '$$dbName' }},"
        "      {$const: 'pipeline_test'}]}"
        "}");

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, expectedExpr);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNsWithExprOnCollFieldPath) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{$expr: {$eq: ['$ns.coll', '" + expCtx->ns.coll() + "']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto case1 =
        "{case: {$in: ['$op', [{$const: 'i' }, {$const: 'u' }, {$const: 'd'}]]}, then: "
        "{$substrBytes: ['$ns', {$add: [{$strLenBytes: ['$$dbName']}, {$const: 1}]}, {$const: "
        "-1}]}}";
    auto case2 = "{case: { $ne: ['$op', {$const: 'c'}]}, then: '$$REMOVE'}";
    auto case3 = "{case: {$ne: ['$o.drop', '$$REMOVE']}, then: '$o.drop'}";
    auto case4 = "{case: {$ne: ['$o.dropDatabase', '$$REMOVE']}, then: '$$REMOVE'}";
    auto case5 =
        "{case: {$ne: ['$o.renameCollection', '$$REMOVE']}, then: {$substrBytes: "
        "['$o.renameCollection', {$add: [{$strLenBytes: ['$$dbName']}, {$const: 1}]}, {$const: "
        "-1}]}}";

    auto expectedExpr = fromjson(
        "{"
        "  $expr: {"
        "    $eq: [{"
        "      $let: {"
        "        vars: {"
        "          dbName: {$substrBytes: ["
        "            '$ns', "
        "            {$const: 0}, "
        "            {$indexOfBytes: ['$ns', {$const: '.'}]}]}}, "
        "        in: {"
        "            $switch: {"
        "              branches: ["s +
        "                " + case1 + ", " + case2 + ", " + case3 + ", " + case4 + ", " + case5 +
        "              ], default: '$$REMOVE'}}}}, "
        "      {$const: 'pipeline_test'}]}"
        "}");

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, expectedExpr);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteNsWithExprOnInvalidFieldPath) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{$expr: {$eq: ['$ns.test', '" + expCtx->ns.coll() + "']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"ns"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$expr: {$eq: ['$$REMOVE', {$const: 'pipeline_test'}]}}"));
}

//
// 'to' rewrites
//
TEST_F(ChangeStreamRewriteTest, CanRewriteFullToObject) {
    auto expCtx = getExpCtx();
    auto statusWithMatchExpression = MatchExpressionParser::parse(
        BSON("to" << BSON("db" << expCtx->ns.db() << "coll" << expCtx->ns.coll())), expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    const std::string ns = expCtx->ns.db().toString() + "." + expCtx->ns.coll().toString();

    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      BSON(AND(fromjson("{op: {$eq: 'c'}}"), BSON("o.to" << BSON("$eq" << ns)))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToObjectWithSwappedField) {
    auto expCtx = getExpCtx();
    auto statusWithMatchExpression = MatchExpressionParser::parse(
        BSON("to" << BSON("coll" << expCtx->ns.coll() << "db" << expCtx->ns.db())), expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      BSON(AND(fromjson("{op: {$eq: 'c'}}"), fromjson("{$alwaysFalse: 1 }"))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToObjectWithOnlyDbField) {
    auto expCtx = getExpCtx();
    auto statusWithMatchExpression =
        MatchExpressionParser::parse(BSON("to" << BSON("db" << expCtx->ns.db())), expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      BSON(AND(fromjson("{op: {$eq: 'c'}}"), fromjson("{$alwaysFalse: 1 }"))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToObjectWithOnlyCollectionField) {
    auto expCtx = getExpCtx();
    auto statusWithMatchExpression =
        MatchExpressionParser::parse(BSON("to" << BSON("coll" << expCtx->ns.coll())), expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      BSON(AND(fromjson("{op: {$eq: 'c'}}"), fromjson("{$alwaysFalse: 1 }"))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToObjectWithInvalidDbField) {
    auto expCtx = getExpCtx();
    auto statusWithMatchExpression = MatchExpressionParser::parse(
        BSON("to" << BSON("db" << 1 << "coll" << expCtx->ns.coll())), expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      BSON(AND(fromjson("{op: {$eq: 'c'}}"), fromjson("{$alwaysFalse: 1 }"))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToObjectWithInvalidCollField) {
    auto expCtx = getExpCtx();
    auto statusWithMatchExpression = MatchExpressionParser::parse(
        BSON("to" << BSON("db" << expCtx->ns.db() << "coll" << 1)), expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      BSON(AND(fromjson("{op: {$eq: 'c'}}"), fromjson("{$alwaysFalse: 1 }"))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToObjectWithExtraField) {
    auto expCtx = getExpCtx();
    auto statusWithMatchExpression = MatchExpressionParser::parse(
        fromjson("{to: {db: 'db', coll: 'coll', extra: 'extra'}}"), expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      BSON(AND(fromjson("{op: {$eq: 'c'}}"), fromjson("{$alwaysFalse: 1 }"))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithStringDbFieldPath) {
    auto expCtx = getExpCtx();
    auto statusWithMatchExpression =
        MatchExpressionParser::parse(BSON("to.db" << expCtx->ns.db()), expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    const std::string regexNs = "^" + expCtx->ns.db().toString() + "\\." +
        DocumentSourceChangeStream::kRegexAllCollections.toString();

    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(AND(fromjson("{op: {$eq: 'c'}}"), BSON("o.to" << BSON("$regex" << regexNs)))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithCollectionFieldPath) {
    auto expCtx = getExpCtx();
    auto statusWithMatchExpression =
        MatchExpressionParser::parse(BSON("to.coll" << expCtx->ns.coll()), expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    const std::string regexNs = DocumentSourceChangeStream::kRegexAllDBs.toString() + "\\." +
        expCtx->ns.coll().toString() + "$";

    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(AND(fromjson("{op: {$eq: 'c'}}"), BSON("o.to" << BSON("$regex" << regexNs)))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithRegexDbFieldPath) {
    auto expCtx = getExpCtx();
    auto statusWithMatchExpression =
        MatchExpressionParser::parse(BSON("to.db" << BSONRegEx(R"(^unit.*$)")), expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                               fromjson(getNsDbRegexMatchExpr("$o.to", R"(^unit.*$)")))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithRegexCollectionFieldPath) {
    auto expCtx = getExpCtx();
    auto statusWithMatchExpression =
        MatchExpressionParser::parse(BSON("to.coll" << BSONRegEx(R"(^pipeline.*$)")), expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                               fromjson(getNsCollRegexMatchExpr("$o.to", R"(^pipeline.*$)")))));
}

TEST_F(ChangeStreamRewriteTest, CannotRewriteToWithInvalidDbFieldPath) {
    auto expCtx = getExpCtx();
    auto statusWithMatchExpression = MatchExpressionParser::parse(BSON("to.db" << 1), expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT_FALSE(rewrittenMatchExpression);
}

TEST_F(ChangeStreamRewriteTest, CannotRewriteToWithInvalidCollectionFieldPath) {
    auto expCtx = getExpCtx();
    auto statusWithMatchExpression = MatchExpressionParser::parse(BSON("to.coll" << 1), expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT_FALSE(rewrittenMatchExpression);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithExtraDbFieldPath) {
    auto expCtx = getExpCtx();
    auto statusWithMatchExpression = MatchExpressionParser::parse(BSON("to.to.subField"
                                                                       << "subDb"),
                                                                  expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      BSON(AND(fromjson("{op: {$eq: 'c'}}"), fromjson("{$alwaysFalse: 1 }"))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithExtraCollectionFieldPath) {
    auto expCtx = getExpCtx();
    auto statusWithMatchExpression = MatchExpressionParser::parse(BSON("to.coll.subField"
                                                                       << "subColl"),
                                                                  expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      BSON(AND(fromjson("{op: {$eq: 'c'}}"), fromjson("{$alwaysFalse: 1 }"))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithInvalidFieldPath) {
    auto expCtx = getExpCtx();
    auto statusWithMatchExpression = MatchExpressionParser::parse(BSON("to.unknown"
                                                                       << "test"),
                                                                  expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        getExpCtx(), statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      BSON(AND(fromjson("{op: {$eq: 'c'}}"), fromjson("{$alwaysFalse: 1 }"))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithInExpressionOnDb) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'to.db': {$in: ['test', 'news']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT(rewrittenMatchExpression);

    const std::string firstRegexNs = std::string("^") + "news" + "\\." +
        DocumentSourceChangeStream::kRegexAllCollections.toString();
    const std::string secondRegexNs = std::string("^") + "test" + "\\." +
        DocumentSourceChangeStream::kRegexAllCollections.toString();

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                               BSON(OR(BSON("o.to" << BSON("$regex" << firstRegexNs)),
                                       BSON("o.to" << BSON("$regex" << secondRegexNs)))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithNinExpressionOnDb) {
    auto expCtx = getExpCtx();
    auto expr = BSON("to.db" << BSON("$nin" << BSON_ARRAY("test"
                                                          << "news")));
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT(rewrittenMatchExpression);

    const std::string firstRegexNs = std::string("^") + "test" + "\\." +
        DocumentSourceChangeStream::kRegexAllCollections.toString();
    const std::string secondRegexNs = std::string("^") + "news" + "\\." +
        DocumentSourceChangeStream::kRegexAllCollections.toString();

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(NOR(BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                          BSON(OR(BSON("o.to" << BSON("$regex" << secondRegexNs)),
                                  BSON("o.to" << BSON("$regex" << firstRegexNs)))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithInExpressionOnCollection) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'to.coll': {$in: ['test', 'news']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT(rewrittenMatchExpression);

    const std::string firstRegexNs =
        DocumentSourceChangeStream::kRegexAllDBs.toString() + "\\." + "news" + "$";
    const std::string secondRegexNs =
        DocumentSourceChangeStream::kRegexAllDBs.toString() + "\\." + "test" + "$";

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                               BSON(OR(BSON("o.to" << BSON("$regex" << firstRegexNs)),
                                       BSON("o.to" << BSON("$regex" << secondRegexNs)))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithNinExpressionOnCollection) {
    auto expCtx = getExpCtx();
    auto expr = BSON("to.coll" << BSON("$nin" << BSON_ARRAY("test"
                                                            << "news")));
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT(rewrittenMatchExpression);

    const std::string firstRegexNs =
        DocumentSourceChangeStream::kRegexAllDBs.toString() + "\\." + "test" + "$";
    const std::string secondRegexNs =
        DocumentSourceChangeStream::kRegexAllDBs.toString() + "\\." + "news" + "$";

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(NOR(BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                          BSON(OR(BSON("o.to" << BSON("$regex" << secondRegexNs)),
                                  BSON("o.to" << BSON("$regex" << firstRegexNs)))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithInRegexExpressionOnDb) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'to.db': {$in: [/^test.*$/, /^news$/]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                               BSON(OR(fromjson(getNsDbRegexMatchExpr("$o.to", R"(^test.*$)")),
                                       fromjson(getNsDbRegexMatchExpr("$o.to", R"(^news$)")))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithNinRegexExpressionOnDb) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'to.db': {$nin: [/^test.*$/, /^news$/]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(NOR(BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                          BSON(OR(fromjson(getNsDbRegexMatchExpr("$o.to", R"(^test.*$)")),
                                  fromjson(getNsDbRegexMatchExpr("$o.to", R"(^news$)")))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithInRegexExpressionOnCollection) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'to.coll': {$in: [/^test.*$/, /^news$/]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                               BSON(OR(fromjson(getNsCollRegexMatchExpr("$o.to", R"(^test.*$)")),
                                       fromjson(getNsCollRegexMatchExpr("$o.to", R"(^news$)")))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithNinRegexExpressionOnCollection) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'to.coll': {$nin: [/^test.*$/, /^news$/]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(NOR(BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                          BSON(OR(fromjson(getNsCollRegexMatchExpr("$o.to", R"(^test.*$)")),
                                  fromjson(getNsCollRegexMatchExpr("$o.to", R"(^news$)")))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithInExpressionOnDbWithRegexAndString) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'to.db': {$in: [/^test.*$/, 'news']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT(rewrittenMatchExpression);

    const std::string secondRegexNs = std::string("^") + "news" + "\\." +
        DocumentSourceChangeStream::kRegexAllCollections.toString();

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                               BSON(OR(BSON("o.to" << BSON("$regex" << secondRegexNs)),
                                       fromjson(getNsDbRegexMatchExpr("$o.to", R"(^test.*$)")))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithNinExpressionOnDbWithRegexAndString) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'to.db': {$nin: [/^test.*$/, 'news']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT(rewrittenMatchExpression);

    const std::string secondRegexNs = std::string("^") + "news" + "\\." +
        DocumentSourceChangeStream::kRegexAllCollections.toString();

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(NOR(BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                          BSON(OR(BSON("o.to" << BSON("$regex" << secondRegexNs)),
                                  fromjson(getNsDbRegexMatchExpr("$o.to", R"(^test.*$)")))))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithInExpressionOnCollectionWithRegexAndString) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'to.coll': {$in: [/^test.*$/, 'news']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT(rewrittenMatchExpression);

    const std::string secondRegexNs =
        DocumentSourceChangeStream::kRegexAllDBs + "\\." + "news" + "$";

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                 BSON(OR(BSON("o.to" << BSON("$regex" << secondRegexNs)),
                         fromjson(getNsCollRegexMatchExpr("$o.to", R"(^test.*$)")))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithNinExpressionOnCollectionWithRegexAndString) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'to.coll': {$nin: [/^test.*$/, 'news']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT(rewrittenMatchExpression);

    const std::string secondRegexNs =
        DocumentSourceChangeStream::kRegexAllDBs + "\\." + "news" + "$";

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(NOR(BSON(AND(fromjson("{op: {$eq: 'c'}}"),
                          BSON(OR(BSON("o.to" << BSON("$regex" << secondRegexNs)),
                                  fromjson(getNsCollRegexMatchExpr("$o.to", R"(^test.*$)")))))))));
}

TEST_F(ChangeStreamRewriteTest, CannotRewriteToWithInExpressionOnInvalidDb) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'to.db': {$in: ['test', 1]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT_FALSE(rewrittenMatchExpression);
}

TEST_F(ChangeStreamRewriteTest, CannotRewriteToWithNinExpressionOnInvalidDb) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'to.db': {$nin: ['test', 1]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT_FALSE(rewrittenMatchExpression);
}

TEST_F(ChangeStreamRewriteTest, CannotRewriteToWithInExpressionOnInvalidCollection) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'to.coll': {$in: ['coll', 1]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT_FALSE(rewrittenMatchExpression);
}

TEST_F(ChangeStreamRewriteTest, CannotRewriteToWithNinExpressionOnInvalidCollection) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'to.coll': {$nin: ['coll', 1]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT_FALSE(rewrittenMatchExpression);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithEmptyInExpression) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'to.db': {$in: []}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      BSON(AND(fromjson("{op: {$eq: 'c'}}"), fromjson("{$alwaysFalse: 1 }"))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithEmptyNinExpression) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'to.db': {$nin: []}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(
        rewrittenPredicate,
        BSON(NOR(BSON(AND(fromjson("{op: {$eq: 'c'}}"), fromjson("{$alwaysFalse: 1 }"))))));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithExprOnFullObject) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{$expr: {$eq: ['$to', {db: '" + expCtx->ns.db() + "', coll: '" +
                         expCtx->ns.coll() + "'}]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT(rewrittenMatchExpression);

    auto expectedExpr = fromjson(
        "{"
        "  $expr: {"
        "    $eq: [{"
        "      $cond: ["
        "        {"
        "          $and: ["
        "            {$eq: ['$op', {$const: 'c'}]},"
        "            {$ne: ['$o.to', '$$REMOVE']}]"
        "        },"
        "        {"
        "          db: {$substrBytes: ["
        "            '$o.to',"
        "            {$const: 0},"
        "            {$indexOfBytes: ['$o.to', {$const: '.'}]}]},"
        "          coll: {$substrBytes: ["
        "            '$o.to',"
        "            {$add: [{$indexOfBytes: ['$o.to', {$const: '.'}]}, {$const: 1}]},"
        "            {$const: -1}]}"
        "        },"
        "        '$$REMOVE']},"
        "      {db: {$const: 'unittests'}, coll: {$const: 'pipeline_test'}}]}}");

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, expectedExpr);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithExprOnDbFieldPath) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{$expr: {$eq: ['$to.db', '" + expCtx->ns.db() + "']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT(rewrittenMatchExpression);

    auto expectedExpr = fromjson(
        "{"
        "  $expr: {"
        "    $eq: [{"
        "      $cond: ["
        "        {"
        "          $and: ["
        "            {$eq: ['$op', {$const: 'c'}]},"
        "            {$ne: ['$o.to', '$$REMOVE']}]"
        "        },"
        "        {"
        "          $substrBytes: ["
        "            '$o.to',"
        "            {$const: 0},"
        "            {$indexOfBytes: ['$o.to', {$const: '.'}]}]"
        "        },"
        "        '$$REMOVE']},"
        "      {$const: 'unittests'}]}}");

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, expectedExpr);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithExprOnCollFieldPath) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{$expr: {$eq: ['$to.coll', '" + expCtx->ns.coll() + "']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT(rewrittenMatchExpression);

    auto expectedExpr = fromjson(
        "{"
        "  $expr: {"
        "    $eq: [{"
        "      $cond: ["
        "        {"
        "          $and: ["
        "            {$eq: ['$op', {$const: 'c'}]},"
        "            {$ne: ['$o.to', '$$REMOVE']}]"
        "        },"
        "        {"
        "          $substrBytes: ["
        "            '$o.to',"
        "            {$add: [{$indexOfBytes: ['$o.to', {$const: '.'}]}, {$const: 1}]},"
        "            {$const: -1}]"
        "        },"
        "        '$$REMOVE']},"
        "      {$const: 'pipeline_test'}]}}");

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate, expectedExpr);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithExprOnInvalidFieldPath) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{$expr: {$eq: ['$to.test', '" + expCtx->ns.coll() + "']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$expr: {$eq: ['$$REMOVE', {$const: 'pipeline_test'}]}}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithExprOnInvalidDbSubFieldPath) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{$expr: {$eq: ['$to.db.test', '" + expCtx->ns.db() + "']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$expr: {$eq: ['$$REMOVE', {$const: 'unittests'}]}}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteToWithExprOnInvalidCollSubFieldPath) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{$expr: {$eq: ['$to.coll.test', '" + expCtx->ns.coll() + "']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"to"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$expr: {$eq: ['$$REMOVE', {$const: 'pipeline_test'}]}}"));
}

//
// 'updateDescription' rewrites
//
TEST_F(ChangeStreamRewriteTest, CanInexactlyRewritePredicateOnFieldUpdateDescription) {
    auto expCtx = getExpCtx();
    auto expr = fromjson(
        "{updateDescription: {updatedFields: {}, removedFields: [], truncatedArrays: []}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    // This produces a minimally selective filter which returns all non-replacement update events.
    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: ["
                               "  {op: {$eq: 'u'}},"
                               "  {'o._id': {$not: {$exists: true}}}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest, CannotExactlyRewritePredicateOnFieldUpdateDescription) {
    auto expCtx = getExpCtx();
    auto expr = fromjson(
        "{updateDescription: {$ne: {updatedFields: {}, removedFields: [], truncatedArrays: []}}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"updateDescription"});

    ASSERT(rewrittenMatchExpression == nullptr);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteEqNullPredicateOnFieldUpdateDescription) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{updateDescription: {$eq: null}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    // Note that this produces an {$alwaysFalse:1} predicate for update events. This will optimize
    // away the enclosing $and so that only non-updates will be returned from the oplog scan.
    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$or: ["
                               "  {$and: ["
                               "    {op: {$eq: 'u'}},"
                               "    {'o._id': {$not: {$exists: true}}},"
                               "    {$alwaysFalse: 1}"
                               "  ]},"
                               "  {$or: ["
                               "    {$and: ["
                               "      {op: {$eq: 'u'}},"
                               "      {'o._id': {$exists: true}}"
                               "    ]},"
                               "    {op: {$not: {$eq: 'u'}}}"
                               "  ]}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteExistsPredicateOnFieldUpdateDescription) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{updateDescription: {$exists: true}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    // Note that the {$alwaysTrue:1} predicate is an artefact of the rewrite process. It will be
    // optimized away and will have no functional impact on the filter.
    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: ["
                               "  {op: {$eq: 'u'}},"
                               "  {'o._id': {$not: {$exists: true}}},"
                               "  {$alwaysTrue: 1}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest, CanInexactlyRewritePredicateOnFieldUpdateDescriptionUpdatedFields) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.updatedFields': {}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    // This produces a minimally selective filter which returns all non-replacement update events.
    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: ["
                               "  {op: {$eq: 'u'}},"
                               "  {'o._id': {$not: {$exists: true}}}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest,
       CannotExactlyRewritePredicateOnFieldUpdateDescriptionUpdatedFields) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.updatedFields': {$ne: {}}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"updateDescription"});

    ASSERT(rewrittenMatchExpression == nullptr);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteEqNullPredicateOnFieldUpdateDescriptionUpdatedFields) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.updatedFields': {$eq: null}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    // Note that this produces an {$alwaysFalse:1} predicate for update events. This will optimize
    // away the enclosing $and so that only non-updates will be returned from the oplog scan.
    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$or: ["
                               "  {$and: ["
                               "    {op: {$eq: 'u'}},"
                               "    {'o._id': {$not: {$exists: true}}},"
                               "    {$alwaysFalse: 1}"
                               "  ]},"
                               "  {$or: ["
                               "    {$and: ["
                               "      {op: {$eq: 'u'}},"
                               "      {'o._id': {$exists: true}}"
                               "    ]},"
                               "    {op: {$not: {$eq: 'u'}}}"
                               "  ]}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteExistsPredicateOnFieldUpdateDescriptionUpdatedFields) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.updatedFields': {$exists: true}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    // Note that the {$alwaysTrue:1} predicate is an artefact of the rewrite process. It will be
    // optimized away and will have no functional impact on the filter.
    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: ["
                               "  {op: {$eq: 'u'}},"
                               "  {'o._id': {$not: {$exists: true}}},"
                               "  {$alwaysTrue: 1}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest,
       CanRewriteArbitraryPredicateOnFieldUpdateDescriptionUpdatedFieldsFoo) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.updatedFields.foo': {$lt: 'b'}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: ["
                               "  {op: {$eq: 'u'}},"
                               "  {'o._id': {$not: {$exists: true}}},"
                               "  {$or: ["
                               "    {'o.diff.i.foo': {$lt: 'b'}},"
                               "    {'o.diff.u.foo': {$lt: 'b'}},"
                               "    {'o.$set.foo': {$lt: 'b'}}"
                               "  ]}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteEqNullPredicateOnFieldUpdateDescriptionUpdatedFieldsFoo) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.updatedFields.foo': {$eq: null}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    // Note that we perform an $and of all three oplog locations for this rewrite.
    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$or: ["
                               "  {$and: ["
                               "    {op: {$eq: 'u'}},"
                               "    {'o._id': {$not: {$exists: true}}},"
                               "    {$and: ["
                               "      {'o.diff.i.foo': {$eq: null}},"
                               "      {'o.diff.u.foo': {$eq: null}},"
                               "      {'o.$set.foo': {$eq: null}}"
                               "    ]}"
                               "  ]},"
                               "  {$or: ["
                               "    {$and: ["
                               "      {op: {$eq: 'u'}},"
                               "      {'o._id': {$exists: true}}"
                               "    ]},"
                               "    {op: {$not: {$eq: 'u'}}}"
                               "  ]}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest,
       CanInexactlyRewriteEqPredicateOnFieldUpdateDescriptionUpdatedFieldsFooBar) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.updatedFields.foo.bar': 'b'}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    // This produces a minimally selective filter which returns all non-replacement update events.
    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: ["
                               "  {op: {$eq: 'u'}},"
                               "  {'o._id': {$not: {$exists: true}}}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest,
       CannotExactlyRewriteEqPredicateOnFieldUpdateDescriptionUpdatedFieldsFooBar) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.updatedFields.foo.bar': {$ne: 'b'}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"updateDescription"});

    ASSERT(rewrittenMatchExpression == nullptr);
}

TEST_F(ChangeStreamRewriteTest,
       CannotRewriteEqNullPredicateOnFieldUpdateDescriptionUpdatedFieldsFooBar) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.updatedFields.foo.bar': {$eq: null}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"updateDescription"});

    ASSERT(rewrittenMatchExpression == nullptr);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteStringEqPredicateOnFieldUpdateDescriptionRemovedFields) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.removedFields': 'z'}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: ["
                               "  {op: {$eq: 'u'}},"
                               "  {'o._id': {$not: {$exists: true}}},"
                               "  {$or: ["
                               "    {'o.diff.d.z': {$exists: true}},"
                               "    {'o.$unset.z': {$exists: true}}"
                               "  ]}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteEqNullPredicateOnFieldUpdateDescriptionRemovedFields) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.removedFields': {$eq: null}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    // Note that this produces an {$alwaysFalse:1} predicate for update events. This will optimize
    // away the enclosing $and so that only non-updates will be returned from the oplog scan.
    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$or: ["
                               "  {$and: ["
                               "    {op: {$eq: 'u'}},"
                               "    {'o._id': {$not: {$exists: true}}},"
                               "    {$alwaysFalse: 1}"
                               "  ]},"
                               "  {$or: ["
                               "    {$and: ["
                               "      {op: {$eq: 'u'}},"
                               "      {'o._id': {$exists: true}}"
                               "    ]},"
                               "    {op: {$not: {$eq: 'u'}}}"
                               "  ]}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteExistsPredicateOnFieldUpdateDescriptionRemovedFields) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.removedFields': {$exists: true}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    // Note that the {$alwaysTrue:1} predicate is an artefact of the rewrite process. It will be
    // optimized away and will have no functional impact on the filter.
    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: ["
                               "  {op: {$eq: 'u'}},"
                               "  {'o._id': {$not: {$exists: true}}},"
                               "  {$alwaysTrue: 1}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest,
       CanInexactlyRewriteDottedStringEqPredicateOnFieldUpdateDescriptionRemovedFields) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.removedFields': 'u.v'}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    // This produces a minimally selective filter which returns all non-replacement update events.
    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: ["
                               "  {op: {$eq: 'u'}},"
                               "  {'o._id': {$not: {$exists: true}}}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest,
       CannotExactlyRewriteDottedStringEqPredicateOnFieldUpdateDescriptionRemovedFields) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.removedFields': {$ne: 'u.v'}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"updateDescription"});

    ASSERT(rewrittenMatchExpression == nullptr);
}

TEST_F(ChangeStreamRewriteTest,
       CanInexactlyRewriteNonStringEqPredicateOnFieldUpdateDescriptionRemovedFields) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.removedFields': ['z']}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    // This produces a minimally selective filter which returns all non-replacement update events.
    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: ["
                               "  {op: {$eq: 'u'}},"
                               "  {'o._id': {$not: {$exists: true}}}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest,
       CannotExactlyRewriteNonStringEqPredicateOnFieldUpdateDescriptionRemovedFields) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.removedFields': {$ne: ['z']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"updateDescription"});

    ASSERT(rewrittenMatchExpression == nullptr);
}

TEST_F(ChangeStreamRewriteTest, CanRewriteStringInPredicateOnFieldUpdateDescriptionRemovedFields) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.removedFields': {$in: ['w', 'y']}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: ["
                               "  {op: {$eq: 'u'}},"
                               "  {'o._id': {$not: {$exists: true}}},"
                               "  {$or: ["
                               "    {$or: ["
                               "        {'o.diff.d.w': {$exists: true}},"
                               "        {'o.$unset.w': {$exists: true}}"
                               "    ]},"
                               "    {$or: ["
                               "        {'o.diff.d.y': {$exists: true}},"
                               "        {'o.$unset.y': {$exists: true}}"
                               "    ]}"
                               "  ]}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest, CanRewriteEmptyInPredicateOnFieldUpdateDescriptionRemovedFields) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.removedFields': {$in: []}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: ["
                               "  {op: {$eq: 'u'}},"
                               "  {'o._id': {$not: {$exists: true}}},"
                               "  {$alwaysFalse: 1}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest,
       CanInexactlyRewriteNonStringInPredicateOnFieldUpdateDescriptionRemovedFields) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.removedFields': {$in: ['w', ['y']]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    // This produces a minimally selective filter which returns all non-replacement update events.
    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: ["
                               "  {op: {$eq: 'u'}},"
                               "  {'o._id': {$not: {$exists: true}}}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest,
       CannotExactlyRewriteNonStringInPredicateOnFieldUpdateDescriptionRemovedFields) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.removedFields': {$nin: ['w', ['y']]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"updateDescription"});

    ASSERT(rewrittenMatchExpression == nullptr);
}

TEST_F(ChangeStreamRewriteTest,
       CanInexactlyRewriteRegexInPredicateOnFieldUpdateDescriptionRemovedFields) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.removedFields': {$in: [/ab*c/, /de*f/]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    // This produces a minimally selective filter which returns all non-replacement update events.
    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: ["
                               "  {op: {$eq: 'u'}},"
                               "  {'o._id': {$not: {$exists: true}}}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest,
       CannotExactlyRewriteRegexInPredicateOnFieldUpdateDescriptionRemovedFields) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.removedFields': {$nin: [/ab*c/, /de*f/]}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"updateDescription"});

    ASSERT(rewrittenMatchExpression == nullptr);
}

TEST_F(ChangeStreamRewriteTest,
       CanInexactlyRewriteArbitraryPredicateOnFieldUpdateDescriptionRemovedFields) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.removedFields': {$lt: 'z'}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    // This produces a minimally selective filter which returns all non-replacement update events.
    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: ["
                               "  {op: {$eq: 'u'}},"
                               "  {'o._id': {$not: {$exists: true}}}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest,
       CannotExactlyRewriteArbitraryPredicateOnFieldUpdateDescriptionRemovedFields) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.removedFields': {$not: {$lt: 'z'}}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"updateDescription"});

    ASSERT(rewrittenMatchExpression == nullptr);
}

TEST_F(ChangeStreamRewriteTest,
       CanInexactlyRewriteArbitraryPredicateOnFieldUpdateDescriptionRemovedFieldsFoo) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.removedFields.foo': {$lt: 'z'}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    // This produces a minimally selective filter which returns all non-replacement update events.
    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: ["
                               "  {op: {$eq: 'u'}},"
                               "  {'o._id': {$not: {$exists: true}}}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest,
       CannotExactlyRewriteArbitraryPredicateOnFieldUpdateDescriptionRemovedFieldsFoo) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.removedFields.foo': {$not: {$lt: 'z'}}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"updateDescription"});

    ASSERT(rewrittenMatchExpression == nullptr);
}

TEST_F(ChangeStreamRewriteTest,
       CanInexactlyRewritePredicateOnFieldUpdateDescriptionTruncatedArrays) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.truncatedArrays': []}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"updateDescription"});
    ASSERT(rewrittenMatchExpression);

    // This produces a minimally selective filter which returns all non-replacement update events.
    auto rewrittenPredicate = rewrittenMatchExpression->serialize();
    ASSERT_BSONOBJ_EQ(rewrittenPredicate,
                      fromjson("{$and: ["
                               "  {op: {$eq: 'u'}},"
                               "  {'o._id': {$not: {$exists: true}}}"
                               "]}"));
}

TEST_F(ChangeStreamRewriteTest,
       CannotExactlyRewritePredicateOnFieldUpdateDescriptionTruncatedArrays) {
    auto expCtx = getExpCtx();
    auto expr = fromjson("{'updateDescription.truncatedArrays': {$ne: []}}");
    auto statusWithMatchExpression = MatchExpressionParser::parse(expr, expCtx);
    ASSERT_OK(statusWithMatchExpression.getStatus());

    auto rewrittenMatchExpression = change_stream_rewrite::rewriteFilterForFields(
        expCtx, statusWithMatchExpression.getValue().get(), {"updateDescription"});

    ASSERT(rewrittenMatchExpression == nullptr);
}

}  // namespace
}  // namespace mongo
