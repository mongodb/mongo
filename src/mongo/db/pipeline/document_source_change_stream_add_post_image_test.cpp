/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/process_interface/stub_lookup_single_document_process_interface.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/tenant_id.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

#include <deque>
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

    // Set up the lookup change post image document source.
    auto lookupChangeDS = DocumentSourceChangeStreamAddPostImage::create(expCtx, getSpec());

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

    auto lookupChangeStage = exec::agg::buildStageAndStitch(lookupChangeDS, mockLocalStage);

    // Mock out the foreign collection.
    getExpCtx()->setMongoProcessInterface(
        std::make_unique<MockMongoInterface>(std::deque<DocumentSource::GetNextResult>{}));

    ASSERT_THROWS_CODE(lookupChangeStage->getNext(), AssertionException, 12840700);
}

TEST_F(DocumentSourceChangeStreamAddPostImageTest, ShouldErrorIfMissingOperationType) {
    auto expCtx = getExpCtx();

    // Set up the lookup change post image document source.
    auto lookupChangeDS = DocumentSourceChangeStreamAddPostImage::create(expCtx, getSpec());

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

    auto lookupChangeStage = exec::agg::buildStageAndStitch(lookupChangeDS, mockLocalStage);

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

    // Set up the lookup change post image document source.
    auto lookupChangeDS = DocumentSourceChangeStreamAddPostImage::create(expCtx, getSpec());

    // Mock its input with a document without a "ns" field.
    auto mockLocalStage = exec::agg::MockStage::createForTest(
        createDocumentWithIdAndResumeToken(0,
                                           Document{
                                               {"documentKey", Document{{"_id", 0}}},
                                               {"operationType", "update"sv},
                                           }),
        expCtx);

    auto lookupChangeStage = exec::agg::buildStageAndStitch(lookupChangeDS, mockLocalStage);

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

    // Set up the lookup change post image document source.
    auto lookupChangeDS = DocumentSourceChangeStreamAddPostImage::create(expCtx, getSpec());

    // Mock its input with a document without a "ns" field.
    auto mockLocalStage = exec::agg::MockStage::createForTest(
        createDocumentWithIdAndResumeToken(0,
                                           Document{{"documentKey", Document{{"_id", 0}}},
                                                    {"operationType", "update"sv},
                                                    {"ns", 4}}),
        expCtx);

    auto lookupChangeStage = exec::agg::buildStageAndStitch(lookupChangeDS, mockLocalStage);

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

    // Set up the lookup change post image document source.
    auto lookupChangeDS = DocumentSourceChangeStreamAddPostImage::create(expCtx, getSpec());

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

    auto lookupChangeStage = exec::agg::buildStageAndStitch(lookupChangeDS, mockLocalStage);

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

    // Set up the lookup change post image document source.
    auto lookupChangeDS = DocumentSourceChangeStreamAddPostImage::create(expCtx, getSpec());

    // Mock its input with a document without a "ns" field.
    auto mockLocalStage = exec::agg::MockStage::createForTest(
        createDocumentWithIdAndResumeToken(
            0,
            Document{{"documentKey", Document{{"_id", 0}}},
                     {"operationType", "update"sv},
                     {"ns", Document{{"db", "DIFFERENT"sv}, {"coll", "irrelevant"sv}}}}),
        expCtx);

    auto lookupChangeStage = exec::agg::buildStageAndStitch(lookupChangeDS, mockLocalStage);

    // Mock out the foreign collection.
    std::deque<DocumentSource::GetNextResult> mockForeignContents{Document{{"_id", 0}}};
    expCtx->setMongoProcessInterface(std::make_unique<MockMongoInterface>(mockForeignContents));

    ASSERT_THROWS_CODE(lookupChangeStage->getNext(), AssertionException, 12840701);
}

TEST_F(DocumentSourceChangeStreamAddPostImageTest, ShouldPassIfDatabaseMatchesOnCollectionlessNss) {
    auto expCtx = getExpCtx();

    expCtx->setNamespaceString(NamespaceString::makeCollectionlessAggregateNSS(
        DatabaseName::createDatabaseName_forTest(boost::none, "test")));

    // Set up the lookup change post image document source.
    auto lookupChangeDS = DocumentSourceChangeStreamAddPostImage::create(expCtx, getSpec());

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

    auto lookupChangeStage = exec::agg::buildStageAndStitch(lookupChangeDS, mockLocalStage);

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

    // Set up the lookup change post image document source.
    auto lookupChangeDS = DocumentSourceChangeStreamAddPostImage::create(expCtx, getSpec());

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

    auto lookupChangeStage = exec::agg::buildStageAndStitch(lookupChangeDS, mockLocalStage);

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

    // Set up the lookup change post image document source.
    auto lookupChangeDS = DocumentSourceChangeStreamAddPostImage::create(expCtx, getSpec());

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

    auto lookupChangeStage = exec::agg::buildStageAndStitch(lookupChangeDS, mockLocalStage);

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
    ASSERT(dynamic_cast<const exec::agg::AggregationSingleDocumentLookupExecutor*>(
        updateLookupStage->getLookupExecutor_forTest()));
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
