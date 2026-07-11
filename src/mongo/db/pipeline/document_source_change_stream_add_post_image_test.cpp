// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_change_stream_add_post_image.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/agg/change_stream_add_post_image_stage.h"
#include "mongo/db/exec/agg/change_stream_update_lookup_stage.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/single_doc_lookup/aggregation_single_document_lookup_executor.h"
#include "mongo/db/exec/single_doc_lookup/express_single_document_lookup_executor.h"
#include "mongo/db/exec/single_doc_lookup/sbe_single_document_lookup_executor.h"
#include "mongo/db/exec/single_doc_lookup/single_document_lookup_executor.h"
#include "mongo/db/exec/single_doc_lookup/single_document_lookup_stats.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/process_interface/stub_lookup_single_document_process_interface.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/tenant_id.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

#include <deque>
#include <limits>
#include <memory>
#include <utility>

#include <boost/none.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

using MockMongoInterface = StubLookupSingleDocumentProcessInterface;

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
class DocumentSourceChangeStreamAddPostImageTest : public AggregationContextFixture {
public:
    /**
     * This method is required to avoid a static initialization fiasco resulting from calling
     * UUID::gen() in file static scope.
     */
    static const UUID& testUuid() {
        static const UUID* uuid_gen = new UUID(UUID::gen());
        return *uuid_gen;
    }

    Document createDocumentWithIdAndResumeToken(int idValue, Document&& doc) {
        MutableDocument combined;

        // Sort key comes first, then _id, then all other fields.
        const Timestamp clusterTime(100, 1);
        ResumeTokenData tokenData(clusterTime,
                                  ResumeTokenData::kDefaultTokenVersion,
                                  /* txnOpIndex */ 0,
                                  testUuid(),
                                  /* eventIdentifier */ Value(Document{{"_id", idValue}}));

        auto resumeTokenDoc = Value(ResumeToken(tokenData).toDocument());
        combined.metadata().setSortKey(resumeTokenDoc, true);
        combined.addField("_id", resumeTokenDoc);

        // Every change event mirrors its resume token's clusterTime in a top-level field; the
        // update-lookup stage reads it from there. Mirror that here so mocks match real events.
        combined.addField(DocumentSourceChangeStream::kClusterTimeField, Value(clusterTime));

        FieldIterator it(doc);
        while (it.more()) {
            auto current = it.next();
            combined.addField(current.first, current.second);
        }

        return combined.freeze();
    }

    DocumentSourceChangeStreamSpec getSpec(
        FullDocumentModeEnum documentMode = FullDocumentModeEnum::kUpdateLookup) {
        auto spec = DocumentSourceChangeStreamSpec();
        spec.setFullDocument(documentMode);
        return spec;
    }

    /**
     * Builds a ChangeStreamUpdateLookupStage directly, with an
     * AggregationSingleDocumentLookupExecutor injected. These tests are about the stage's document-
     * transformation logic, which is executor-agnostic, so they inject the Aggregation executor
     * directly and run entirely against the mocked MongoProcessInterface.
     */
    boost::intrusive_ptr<exec::agg::Stage> buildUpdateLookupStage(
        const boost::intrusive_ptr<exec::agg::Stage>& source) {
        auto stage = make_intrusive<exec::agg::ChangeStreamUpdateLookupStage>(
            DocumentSourceChangeStreamAddPostImage::kStageName,
            getExpCtx(),
            std::make_unique<exec::agg::AggregationSingleDocumentLookupExecutor>(
                exec::SingleDocumentLookupStatsRecorder::makeUpdateLookupAggregationRecorder()),
            exec::agg::BatchedEnrichmentStage::Limits{
                .maxInputEvents = 1,
                .maxInputBytes = std::numeric_limits<size_t>::max(),
                .maxOutputBytes = std::numeric_limits<size_t>::max()});
        exec::agg::stitchStage(*stage, source.get());
        return stage;
    }
};

TEST_F(DocumentSourceChangeStreamAddPostImageTest,
       CannotCreateStageFromBsonWithUnrecognizedFullDocumentMode) {
    auto expCtx = getExpCtx();
    auto spec = BSON("$changeStream: " << BSON("fullDocument" << "banana"));
    ASSERT_THROWS_CODE(
        DocumentSourceChangeStreamAddPostImage::createFromBson(spec.firstElement(), expCtx),
        AssertionException,
        ErrorCodes::BadValue);
}

TEST_F(DocumentSourceChangeStreamAddPostImageTest, ShouldSerializeAsExpectedForExplain) {
    auto expCtx = getExpCtx();
    const auto stage = DocumentSourceChangeStreamAddPostImage::create(
        expCtx, getSpec(FullDocumentModeEnum::kUpdateLookup));
    const auto expectedOutput =
        Value(Document{{DocumentSourceChangeStream::kStageName,
                        Document{{"stage"sv, DocumentSourceChangeStreamAddPostImage::kStageName},
                                 {"fullDocument"sv, "updateLookup"sv}}}});

    ASSERT_VALUE_EQ(
        stage->serialize(query_shape::SerializationOptions{
            .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)}),
        expectedOutput);
}

TEST_F(DocumentSourceChangeStreamAddPostImageTest, ShouldSerializeAsExpectedForDispatch) {
    auto expCtx = getExpCtx();
    const auto stage = DocumentSourceChangeStreamAddPostImage::create(
        expCtx, getSpec(FullDocumentModeEnum::kUpdateLookup));
    const auto expectedOutput = Value(Document{{DocumentSourceChangeStreamAddPostImage::kStageName,
                                                Document{{"fullDocument"sv, "updateLookup"sv}}}});

    ASSERT_VALUE_EQ(stage->serialize(), expectedOutput);
}

TEST_F(DocumentSourceChangeStreamAddPostImageTest, ShouldErrorIfMissingDocumentKeyOnUpdate) {
    auto expCtx = getExpCtx();

    // Mock its input with a document without a "documentKey" field.
    auto mockLocalStage = exec::agg::MockStage::createForTest(
        {createDocumentWithIdAndResumeToken(
            0,
            Document{{"operationType", "update"sv},
                     {"fullDocument", Document{{"_id", 0}}},
                     {"ns",
                      Document{{"db", expCtx->getNamespaceString().db_forTest()},
                               {"coll", expCtx->getNamespaceString().coll()}}}})},
        expCtx);

    auto lookupChangeStage = buildUpdateLookupStage(mockLocalStage);

    // Mock out the foreign collection.
    getExpCtx()->setMongoProcessInterface(
        std::make_unique<MockMongoInterface>(std::deque<DocumentSource::GetNextResult>{}));

    ASSERT_THROWS_CODE(lookupChangeStage->getNext(), AssertionException, 12840700);
}

TEST_F(DocumentSourceChangeStreamAddPostImageTest, ShouldErrorIfMissingOperationType) {
    auto expCtx = getExpCtx();

    // Mock its input with a document without a "ns" field.
    auto mockLocalStage = exec::agg::MockStage::createForTest(
        createDocumentWithIdAndResumeToken(
            0,
            Document{{"documentKey", Document{{"_id", 0}}},
                     {"fullDocument", Document{{"_id", 0}}},
                     {"ns",
                      Document{{"db", expCtx->getNamespaceString().db_forTest()},
                               {"coll", expCtx->getNamespaceString().coll()}}}}),
        expCtx);

    auto lookupChangeStage = buildUpdateLookupStage(mockLocalStage);

    // Mock out the foreign collection.
    getExpCtx()->setMongoProcessInterface(
        std::make_unique<MockMongoInterface>(std::deque<DocumentSource::GetNextResult>{}));

    ASSERT_THROWS_CODE(lookupChangeStage->getNext(), AssertionException, 12840700);
}

TEST_F(DocumentSourceChangeStreamAddPostImageTest, ShouldErrorIfMissingNamespace) {
    auto expCtx = getExpCtx();

    // A collection-level stream looks up its fixed namespace and skips the per-event 'ns' parse, so
    // namespace validation runs only for db/cluster-wide streams. Use a collectionless (db-level)
    // stream so the event's 'ns' is parsed and the missing field is rejected.
    expCtx->setNamespaceString(NamespaceString::makeCollectionlessAggregateNSS(
        DatabaseName::createDatabaseName_forTest(boost::none, "test")));

    // Mock its input with a document without a "ns" field.
    auto mockLocalStage = exec::agg::MockStage::createForTest(
        createDocumentWithIdAndResumeToken(0,
                                           Document{
                                               {"documentKey", Document{{"_id", 0}}},
                                               {"operationType", "update"sv},
                                           }),
        expCtx);

    auto lookupChangeStage = buildUpdateLookupStage(mockLocalStage);

    // Mock out the foreign collection.
    getExpCtx()->setMongoProcessInterface(
        std::make_unique<MockMongoInterface>(std::deque<DocumentSource::GetNextResult>{}));

    ASSERT_THROWS_CODE(lookupChangeStage->getNext(), AssertionException, 12840700);
}

TEST_F(DocumentSourceChangeStreamAddPostImageTest, ShouldErrorIfNsFieldHasWrongType) {
    auto expCtx = getExpCtx();

    // A collection-level stream looks up its fixed namespace and skips the per-event 'ns' parse, so
    // namespace validation runs only for db/cluster-wide streams. Use a collectionless (db-level)
    // stream so the event's 'ns' is parsed and the wrong-typed field is rejected.
    expCtx->setNamespaceString(NamespaceString::makeCollectionlessAggregateNSS(
        DatabaseName::createDatabaseName_forTest(boost::none, "test")));

    // Mock its input with a document without a "ns" field.
    auto mockLocalStage = exec::agg::MockStage::createForTest(
        createDocumentWithIdAndResumeToken(0,
                                           Document{{"documentKey", Document{{"_id", 0}}},
                                                    {"operationType", "update"sv},
                                                    {"ns", 4}}),
        expCtx);

    auto lookupChangeStage = buildUpdateLookupStage(mockLocalStage);

    // Mock out the foreign collection.
    getExpCtx()->setMongoProcessInterface(
        std::make_unique<MockMongoInterface>(std::deque<DocumentSource::GetNextResult>{}));

    ASSERT_THROWS_CODE(lookupChangeStage->getNext(), AssertionException, 12840700);
}

TEST_F(DocumentSourceChangeStreamAddPostImageTest, ShouldErrorIfNsFieldDoesNotMatchPipeline) {
    auto expCtx = getExpCtx();

    // A collection-level stream looks up its fixed namespace and skips the per-event 'ns' parse, so
    // namespace validation runs only for db/cluster-wide streams. Use a collectionless (db-level)
    // stream so the event's 'ns' (in a different database) is parsed and rejected.
    expCtx->setNamespaceString(NamespaceString::makeCollectionlessAggregateNSS(
        DatabaseName::createDatabaseName_forTest(boost::none, "test")));

    // Mock its input with a document without a "ns" field.
    auto mockLocalStage = exec::agg::MockStage::createForTest(
        createDocumentWithIdAndResumeToken(
            0,
            Document{
                {"documentKey", Document{{"_id", 0}}},
                {"operationType", "update"sv},
                {"ns",
                 Document{{"db", "DIFFERENT"sv}, {"coll", expCtx->getNamespaceString().coll()}}}}),
        expCtx);

    auto lookupChangeStage = buildUpdateLookupStage(mockLocalStage);

    // Mock out the foreign collection.
    getExpCtx()->setMongoProcessInterface(
        std::make_unique<MockMongoInterface>(std::deque<DocumentSource::GetNextResult>{}));

    ASSERT_THROWS_CODE(lookupChangeStage->getNext(), AssertionException, 12840701);
}

TEST_F(DocumentSourceChangeStreamAddPostImageTest,
       ShouldErrorIfDatabaseMismatchOnCollectionlessNss) {
    auto expCtx = getExpCtx();

    expCtx->setNamespaceString(NamespaceString::makeCollectionlessAggregateNSS(
        DatabaseName::createDatabaseName_forTest(boost::none, "test")));

    // Mock its input with a document without a "ns" field.
    auto mockLocalStage = exec::agg::MockStage::createForTest(
        createDocumentWithIdAndResumeToken(
            0,
            Document{{"documentKey", Document{{"_id", 0}}},
                     {"operationType", "update"sv},
                     {"ns", Document{{"db", "DIFFERENT"sv}, {"coll", "irrelevant"sv}}}}),
        expCtx);

    auto lookupChangeStage = buildUpdateLookupStage(mockLocalStage);

    // Mock out the foreign collection.
    std::deque<DocumentSource::GetNextResult> mockForeignContents{Document{{"_id", 0}}};
    expCtx->setMongoProcessInterface(std::make_unique<MockMongoInterface>(mockForeignContents));

    ASSERT_THROWS_CODE(lookupChangeStage->getNext(), AssertionException, 12840701);
}

TEST_F(DocumentSourceChangeStreamAddPostImageTest, ShouldPassIfDatabaseMatchesOnCollectionlessNss) {
    auto expCtx = getExpCtx();

    expCtx->setNamespaceString(NamespaceString::makeCollectionlessAggregateNSS(
        DatabaseName::createDatabaseName_forTest(boost::none, "test")));

    // Mock out the foreign collection.
    std::deque<DocumentSource::GetNextResult> mockForeignContents{Document{{"_id", 0}}};
    expCtx->setMongoProcessInterface(std::make_unique<MockMongoInterface>(mockForeignContents));

    auto mockLocalStage = exec::agg::MockStage::createForTest(
        createDocumentWithIdAndResumeToken(
            0,
            Document{{"documentKey", Document{{"_id", 0}}},
                     {"operationType", "update"sv},
                     {"ns",
                      Document{{"db", expCtx->getNamespaceString().db_forTest()},
                               {"coll", "irrelevant"sv}}}}),
        expCtx);

    auto lookupChangeStage = buildUpdateLookupStage(mockLocalStage);

    auto next = lookupChangeStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(),
                       createDocumentWithIdAndResumeToken(
                           0,
                           Document{{"documentKey", Document{{"_id", 0}}},
                                    {"operationType", "update"sv},
                                    {"ns",
                                     Document{{"db", expCtx->getNamespaceString().db_forTest()},
                                              {"coll", "irrelevant"sv}}},
                                    {"fullDocument", Document{{"_id", 0}}}}));
}

TEST_F(DocumentSourceChangeStreamAddPostImageTest, ShouldErrorIfDocumentKeyIsNotUnique) {
    auto expCtx = getExpCtx();

    // Mock its input with an update document.
    auto mockLocalStage = exec::agg::MockStage::createForTest(
        createDocumentWithIdAndResumeToken(
            0,
            Document{{"documentKey", Document{{"_id", 0}}},
                     {"operationType", "update"sv},
                     {"ns",
                      Document{{"db", expCtx->getNamespaceString().db_forTest()},
                               {"coll", expCtx->getNamespaceString().coll()}}}}),
        expCtx);

    auto lookupChangeStage = buildUpdateLookupStage(mockLocalStage);

    // Mock out the foreign collection to have two documents with the same document key.
    std::deque<DocumentSource::GetNextResult> foreignCollection = {Document{{"_id", 0}},
                                                                   Document{{"_id", 0}}};
    getExpCtx()->setMongoProcessInterface(
        std::make_unique<MockMongoInterface>(std::move(foreignCollection)));

    ASSERT_THROWS_CODE(
        lookupChangeStage->getNext(), AssertionException, ErrorCodes::ChangeStreamFatalError);
}

TEST_F(DocumentSourceChangeStreamAddPostImageTest, ShouldPropagatePauses) {
    auto expCtx = getExpCtx();

    // Mock its input, pausing every other result.
    auto mockLocalStage = exec::agg::MockStage::createForTest(
        {createDocumentWithIdAndResumeToken(
             0,
             Document{{"documentKey", Document{{"_id", 0}}},
                      {"operationType", "insert"sv},
                      {"ns",
                       Document{{"db", expCtx->getNamespaceString().db_forTest()},
                                {"coll", expCtx->getNamespaceString().coll()}}},
                      {"fullDocument", Document{{"_id", 0}}}}),
         DocumentSource::GetNextResult::makePauseExecution(),
         createDocumentWithIdAndResumeToken(
             1,
             Document{{"documentKey", Document{{"_id", 1}}},
                      {"operationType", "update"sv},
                      {"ns",
                       Document{{"db", expCtx->getNamespaceString().db_forTest()},
                                {"coll", expCtx->getNamespaceString().coll()}}}}),
         DocumentSource::GetNextResult::makePauseExecution()},
        expCtx);

    auto lookupChangeStage = buildUpdateLookupStage(mockLocalStage);

    // Mock out the foreign collection.
    std::deque<DocumentSource::GetNextResult> mockForeignContents{Document{{"_id", 0}},
                                                                  Document{{"_id", 1}}};
    getExpCtx()->setMongoProcessInterface(
        std::make_unique<MockMongoInterface>(std::move(mockForeignContents)));

    auto next = lookupChangeStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(),
                       createDocumentWithIdAndResumeToken(
                           0,
                           Document{{"documentKey", Document{{"_id", 0}}},
                                    {"operationType", "insert"sv},
                                    {"ns",
                                     Document{{"db", expCtx->getNamespaceString().db_forTest()},
                                              {"coll", expCtx->getNamespaceString().coll()}}},
                                    {"fullDocument", Document{{"_id", 0}}}}));

    ASSERT_TRUE(lookupChangeStage->getNext().isPaused());

    next = lookupChangeStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(),
                       createDocumentWithIdAndResumeToken(
                           1,
                           Document{{"documentKey", Document{{"_id", 1}}},
                                    {"operationType", "update"sv},
                                    {"ns",
                                     Document{{"db", expCtx->getNamespaceString().db_forTest()},
                                              {"coll", expCtx->getNamespaceString().coll()}}},
                                    {"fullDocument", Document{{"_id", 1}}}}));

    ASSERT_TRUE(lookupChangeStage->getNext().isPaused());

    ASSERT_TRUE(lookupChangeStage->getNext().isEOF());
    ASSERT_TRUE(lookupChangeStage->getNext().isEOF());
}

TEST_F(DocumentSourceChangeStreamAddPostImageTest,
       MatchCollectionUUIDReadsAndStripsCollectionUUIDField) {
    auto expCtx = getExpCtx();
    auto spec = getSpec();
    spec.setMatchCollectionUUIDForUpdateLookup(true);
    expCtx->setChangeStreamSpec(spec);

    // The event carries the collection UUID emitted by the event transform.
    auto mockLocalStage = exec::agg::MockStage::createForTest(
        createDocumentWithIdAndResumeToken(
            0,
            Document{{"documentKey", Document{{"_id", 0}}},
                     {"operationType", "update"sv},
                     {std::string{DocumentSourceChangeStream::kCollectionUuidField}, testUuid()},
                     {"ns",
                      Document{{"db", expCtx->getNamespaceString().db_forTest()},
                               {"coll", expCtx->getNamespaceString().coll()}}}}),
        expCtx);

    auto lookupChangeStage = buildUpdateLookupStage(mockLocalStage);

    std::deque<DocumentSource::GetNextResult> mockForeignContents{Document{{"_id", 0}}};
    expCtx->setMongoProcessInterface(std::make_unique<MockMongoInterface>(mockForeignContents));

    auto next = lookupChangeStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    auto outputDoc = next.releaseDocument();

    // The lookup succeeded and the internal-only 'collectionUUID' field was removed.
    ASSERT_VALUE_EQ(outputDoc[DocumentSourceChangeStreamAddPostImage::kFullDocumentFieldName],
                    Value(Document{{"_id", 0}}));
    ASSERT_TRUE(outputDoc[DocumentSourceChangeStream::kCollectionUuidField].missing());
}

TEST_F(DocumentSourceChangeStreamAddPostImageTest,
       ShowExpandedEventsKeepsCollectionUUIDFieldOnOutput) {
    auto expCtx = getExpCtx();
    auto spec = getSpec();
    spec.setMatchCollectionUUIDForUpdateLookup(true);
    spec.setShowExpandedEvents(true);
    expCtx->setChangeStreamSpec(spec);

    auto mockLocalStage = exec::agg::MockStage::createForTest(
        createDocumentWithIdAndResumeToken(
            0,
            Document{{"documentKey", Document{{"_id", 0}}},
                     {"operationType", "update"sv},
                     {std::string{DocumentSourceChangeStream::kCollectionUuidField}, testUuid()},
                     {"ns",
                      Document{{"db", expCtx->getNamespaceString().db_forTest()},
                               {"coll", expCtx->getNamespaceString().coll()}}}}),
        expCtx);

    auto lookupChangeStage = buildUpdateLookupStage(mockLocalStage);

    std::deque<DocumentSource::GetNextResult> mockForeignContents{Document{{"_id", 0}}};
    expCtx->setMongoProcessInterface(std::make_unique<MockMongoInterface>(mockForeignContents));

    auto next = lookupChangeStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    auto outputDoc = next.releaseDocument();

    // With 'showExpandedEvents' the 'collectionUUID' field is user-visible and must remain.
    ASSERT_VALUE_EQ(outputDoc[DocumentSourceChangeStream::kCollectionUuidField], Value(testUuid()));
}

using DocumentSourceChangeStreamAddPostImageDeathTest = DocumentSourceChangeStreamAddPostImageTest;

DEATH_TEST_REGEX_F(DocumentSourceChangeStreamAddPostImageDeathTest,
                   MatchCollectionUUIDWithoutCollectionUUIDFieldTasserts,
                   "Tripwire assertion.*12888200") {
    auto expCtx = getExpCtx();
    auto spec = getSpec();
    spec.setMatchCollectionUUIDForUpdateLookup(true);
    expCtx->setChangeStreamSpec(spec);

    // The event lacks the 'collectionUUID' field the transform must have emitted.
    auto mockLocalStage = exec::agg::MockStage::createForTest(
        createDocumentWithIdAndResumeToken(
            0,
            Document{{"documentKey", Document{{"_id", 0}}},
                     {"operationType", "update"sv},
                     {"ns",
                      Document{{"db", expCtx->getNamespaceString().db_forTest()},
                               {"coll", expCtx->getNamespaceString().coll()}}}}),
        expCtx);

    auto lookupChangeStage = buildUpdateLookupStage(mockLocalStage);

    std::deque<DocumentSource::GetNextResult> mockForeignContents{Document{{"_id", 0}}};
    expCtx->setMongoProcessInterface(std::make_unique<MockMongoInterface>(mockForeignContents));

    ASSERT_THROWS_CODE(lookupChangeStage->getNext(), AssertionException, 12888200);
}

TEST_F(DocumentSourceChangeStreamAddPostImageTest, UpdateLookupDependsOnCollectionUUIDField) {
    auto ds = DocumentSourceChangeStreamAddPostImage::create(getExpCtx(), getSpec());
    DepsTracker deps;
    ds->getDependencies(&deps);
    ASSERT_EQ(deps.fields.count(std::string{DocumentSourceChangeStream::kCollectionUuidField}), 1u);
}

// Wire-up tests: assert documentSourceChangeStreamAddPostImageToStageFn builds the right
// single-responsibility execution stage for each 'fullDocument' mode. This guards the split this
// refactor introduced:
// - 'updateLookup' -> ChangeStreamUpdateLookupStage
// - 'required' / 'whenAvailable' -> ChangeStreamAddPostImageStage.
class ChangeStreamAddPostImageStageFnWiringTest
    : public DocumentSourceChangeStreamAddPostImageTest {
protected:
    boost::intrusive_ptr<exec::agg::Stage> buildStageForMode(FullDocumentModeEnum mode) {
        auto ds = DocumentSourceChangeStreamAddPostImage::create(getExpCtx(), getSpec(mode));
        return exec::agg::buildStage(ds);
    }
};

TEST_F(ChangeStreamAddPostImageStageFnWiringTest, UpdateLookupModeBuildsUpdateLookupStage) {
    auto stage = buildStageForMode(FullDocumentModeEnum::kUpdateLookup);
    auto* updateLookupStage = dynamic_cast<exec::agg::ChangeStreamUpdateLookupStage*>(stage.get());
    ASSERT(updateLookupStage);
    ASSERT_FALSE(dynamic_cast<exec::agg::ChangeStreamAddPostImageStage*>(stage.get()));
}

// Flag off (default): the updateLookup stage routes through the Aggregation fallback alone.
TEST_F(ChangeStreamAddPostImageStageFnWiringTest, UpdateLookupFlagOffWiresAggregationOnly) {
    unittest::ServerParameterGuard flag{"featureFlagChangeStreamOptimizedUpdateLookup", false};
    auto stage = buildStageForMode(FullDocumentModeEnum::kUpdateLookup);
    auto* updateLookupStage = dynamic_cast<exec::agg::ChangeStreamUpdateLookupStage*>(stage.get());
    ASSERT(updateLookupStage);
    ASSERT(dynamic_cast<const exec::agg::AggregationSingleDocumentLookupExecutor*>(
        updateLookupStage->getLookupExecutor_forTest()));
}

// Flag on, collection-level stream (the fixture's default namespace): the updateLookup stage
// routes through PrimaryWithFallback(Sbe, Aggregation).
TEST_F(ChangeStreamAddPostImageStageFnWiringTest,
       UpdateLookupFlagOnWiresSbeWithAggregationFallbackForCollectionStream) {
    unittest::ServerParameterGuard flag{"featureFlagChangeStreamOptimizedUpdateLookup", true};
    auto stage = buildStageForMode(FullDocumentModeEnum::kUpdateLookup);
    auto* updateLookupStage = dynamic_cast<exec::agg::ChangeStreamUpdateLookupStage*>(stage.get());
    ASSERT(updateLookupStage);

    auto* chain = dynamic_cast<const exec::agg::PrimaryWithFallbackSingleDocumentLookupExecutor*>(
        updateLookupStage->getLookupExecutor_forTest());
    ASSERT(chain);
    ASSERT(
        dynamic_cast<const exec::agg::SbeSingleDocumentLookupExecutor*>(chain->primary_forTest()));
    ASSERT(dynamic_cast<const exec::agg::AggregationSingleDocumentLookupExecutor*>(
        chain->fallback_forTest()));
}

// Flag on, database-level stream: the updateLookup stage routes through
// PrimaryWithFallback(Express, Aggregation).
TEST_F(ChangeStreamAddPostImageStageFnWiringTest,
       UpdateLookupFlagOnWiresExpressWithAggregationFallbackForDatabaseStream) {
    unittest::ServerParameterGuard flag{"featureFlagChangeStreamOptimizedUpdateLookup", true};
    getExpCtx()->setNamespaceString(NamespaceString::makeCollectionlessAggregateNSS(
        getExpCtx()->getNamespaceString().dbName()));
    auto stage = buildStageForMode(FullDocumentModeEnum::kUpdateLookup);
    auto* updateLookupStage = dynamic_cast<exec::agg::ChangeStreamUpdateLookupStage*>(stage.get());
    ASSERT(updateLookupStage);

    auto* chain = dynamic_cast<const exec::agg::PrimaryWithFallbackSingleDocumentLookupExecutor*>(
        updateLookupStage->getLookupExecutor_forTest());
    ASSERT(chain);
    ASSERT(dynamic_cast<const exec::agg::ExpressSingleDocumentLookupExecutor*>(
        chain->primary_forTest()));
    ASSERT(dynamic_cast<const exec::agg::AggregationSingleDocumentLookupExecutor*>(
        chain->fallback_forTest()));
}

TEST_F(ChangeStreamAddPostImageStageFnWiringTest, RequiredModeBuildsAddPostImageStage) {
    auto stage = buildStageForMode(FullDocumentModeEnum::kRequired);
    ASSERT(dynamic_cast<exec::agg::ChangeStreamAddPostImageStage*>(stage.get()));
    ASSERT_FALSE(dynamic_cast<exec::agg::ChangeStreamUpdateLookupStage*>(stage.get()));
}

TEST_F(ChangeStreamAddPostImageStageFnWiringTest, WhenAvailableModeBuildsAddPostImageStage) {
    auto stage = buildStageForMode(FullDocumentModeEnum::kWhenAvailable);
    ASSERT(dynamic_cast<exec::agg::ChangeStreamAddPostImageStage*>(stage.get()));
    ASSERT_FALSE(dynamic_cast<exec::agg::ChangeStreamUpdateLookupStage*>(stage.get()));
}

}  // namespace
}  // namespace mongo
