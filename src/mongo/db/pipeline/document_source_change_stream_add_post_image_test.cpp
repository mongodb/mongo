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
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_mock.h"
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
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {
using std::deque;

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

    Document makeResumeToken(ImplicitValue id = Value()) {
        const Timestamp ts(100, 1);
        if (id.missing()) {
            ResumeTokenData tokenData(ts,
                                      ResumeTokenData::kDefaultTokenVersion,
                                      /* txnOpIndex */ 0,
                                      /* uuid */ boost::none,
                                      /* eventIdentifier */ Value());
            return ResumeToken(tokenData).toDocument();
        }
        ResumeTokenData tokenData(ts,
                                  /* version */ 0,
                                  /* txnOpIndex */ 0,
                                  testUuid(),
                                  /* eventIdentifier */ Value(Document{{"_id", id}}));
        return ResumeToken(tokenData).toDocument();
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
                        Document{{"stage"_sd, DocumentSourceChangeStreamAddPostImage::kStageName},
                                 {"fullDocument"_sd, "updateLookup"_sd}}}});

    ASSERT_VALUE_EQ(
        stage->serialize(SerializationOptions{
            .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)}),
        expectedOutput);
}

TEST_F(DocumentSourceChangeStreamAddPostImageTest, ShouldSerializeAsExpectedForDispatch) {
    auto expCtx = getExpCtx();
    const auto stage = DocumentSourceChangeStreamAddPostImage::create(
        expCtx, getSpec(FullDocumentModeEnum::kUpdateLookup));
    const auto expectedOutput = Value(Document{{DocumentSourceChangeStreamAddPostImage::kStageName,
                                                Document{{"fullDocument"_sd, "updateLookup"_sd}}}});

    ASSERT_VALUE_EQ(stage->serialize(), expectedOutput);
}

TEST_F(DocumentSourceChangeStreamAddPostImageTest, ShouldErrorIfMissingDocumentKeyOnUpdate) {
    auto expCtx = getExpCtx();

    // Set up the lookup change post image document source.
    auto lookupChangeDS = DocumentSourceChangeStreamAddPostImage::create(expCtx, getSpec());

    // Mock its input with a document without a "documentKey" field.
    auto mockLocalStage = exec::agg::MockStage::createForTest(
        Document{{"_id", makeResumeToken(0)},
                 {"operationType", "update"_sd},
                 {"fullDocument", Document{{"_id", 0}}},
                 {"ns",
                  Document{{"db", expCtx->getNamespaceString().db_forTest()},
                           {"coll", expCtx->getNamespaceString().coll()}}}},
        expCtx);

    auto lookupChangeStage = exec::agg::buildStage(lookupChangeDS);
    lookupChangeStage->setSource(mockLocalStage.get());

    // Mock out the foreign collection.
    getExpCtx()->setMongoProcessInterface(
        std::make_unique<MockMongoInterface>(deque<DocumentSource::GetNextResult>{}));

    ASSERT_THROWS_CODE(lookupChangeStage->getNext(), AssertionException, 40578);
}

TEST_F(DocumentSourceChangeStreamAddPostImageTest, ShouldErrorIfMissingOperationType) {
    auto expCtx = getExpCtx();

    // Set up the lookup change post image document source.
    auto lookupChangeDS = DocumentSourceChangeStreamAddPostImage::create(expCtx, getSpec());

    // Mock its input with a document without a "ns" field.
    auto mockLocalStage = exec::agg::MockStage::createForTest(
        Document{{"_id", makeResumeToken(0)},
                 {"documentKey", Document{{"_id", 0}}},
                 {"fullDocument", Document{{"_id", 0}}},
                 {"ns",
                  Document{{"db", expCtx->getNamespaceString().db_forTest()},
                           {"coll", expCtx->getNamespaceString().coll()}}}},
        expCtx);

    auto lookupChangeStage = exec::agg::buildStage(lookupChangeDS);
    lookupChangeStage->setSource(mockLocalStage.get());

    // Mock out the foreign collection.
    getExpCtx()->setMongoProcessInterface(
        std::make_unique<MockMongoInterface>(deque<DocumentSource::GetNextResult>{}));

    ASSERT_THROWS_CODE(lookupChangeStage->getNext(), AssertionException, 40578);
}

TEST_F(DocumentSourceChangeStreamAddPostImageTest, ShouldErrorIfMissingNamespace) {
    auto expCtx = getExpCtx();

    // Set up the lookup change post image document source.
    auto lookupChangeDS = DocumentSourceChangeStreamAddPostImage::create(expCtx, getSpec());

    // Mock its input with a document without a "ns" field.
    auto mockLocalStage = exec::agg::MockStage::createForTest(
        Document{
            {"_id", makeResumeToken(0)},
            {"documentKey", Document{{"_id", 0}}},
            {"operationType", "update"_sd},
        },
        expCtx);

    auto lookupChangeStage = exec::agg::buildStage(lookupChangeDS);
    lookupChangeStage->setSource(mockLocalStage.get());

    // Mock out the foreign collection.
    getExpCtx()->setMongoProcessInterface(
        std::make_unique<MockMongoInterface>(deque<DocumentSource::GetNextResult>{}));

    ASSERT_THROWS_CODE(lookupChangeStage->getNext(), AssertionException, 40578);
}

TEST_F(DocumentSourceChangeStreamAddPostImageTest, ShouldErrorIfNsFieldHasWrongType) {
    auto expCtx = getExpCtx();

    // Set up the lookup change post image document source.
    auto lookupChangeDS = DocumentSourceChangeStreamAddPostImage::create(expCtx, getSpec());

    // Mock its input with a document without a "ns" field.
    auto mockLocalStage =
        exec::agg::MockStage::createForTest(Document{{"_id", makeResumeToken(0)},
                                                     {"documentKey", Document{{"_id", 0}}},
                                                     {"operationType", "update"_sd},
                                                     {"ns", 4}},
                                            expCtx);

    auto lookupChangeStage = exec::agg::buildStage(lookupChangeDS);
    lookupChangeStage->setSource(mockLocalStage.get());

    // Mock out the foreign collection.
    getExpCtx()->setMongoProcessInterface(
        std::make_unique<MockMongoInterface>(deque<DocumentSource::GetNextResult>{}));

    ASSERT_THROWS_CODE(lookupChangeStage->getNext(), AssertionException, 40578);
}

TEST_F(DocumentSourceChangeStreamAddPostImageTest, ShouldErrorIfNsFieldDoesNotMatchPipeline) {
    auto expCtx = getExpCtx();

    // Set up the lookup change post image document source.
    auto lookupChangeDS = DocumentSourceChangeStreamAddPostImage::create(expCtx, getSpec());

    // Mock its input with a document without a "ns" field.
    auto mockLocalStage = exec::agg::MockStage::createForTest(
        Document{{"_id", makeResumeToken(0)},
                 {"documentKey", Document{{"_id", 0}}},
                 {"operationType", "update"_sd},
                 {"ns",
                  Document{{"db", "DIFFERENT"_sd}, {"coll", expCtx->getNamespaceString().coll()}}}},
        expCtx);

    auto lookupChangeStage = exec::agg::buildStage(lookupChangeDS);
    lookupChangeStage->setSource(mockLocalStage.get());

    // Mock out the foreign collection.
    getExpCtx()->setMongoProcessInterface(
        std::make_unique<MockMongoInterface>(deque<DocumentSource::GetNextResult>{}));

    ASSERT_THROWS_CODE(lookupChangeStage->getNext(), AssertionException, 40579);
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
        Document{{"_id", makeResumeToken(0)},
                 {"documentKey", Document{{"_id", 0}}},
                 {"operationType", "update"_sd},
                 {"ns", Document{{"db", "DIFFERENT"_sd}, {"coll", "irrelevant"_sd}}}},
        expCtx);

    auto lookupChangeStage = exec::agg::buildStage(lookupChangeDS);
    lookupChangeStage->setSource(mockLocalStage.get());

    // Mock out the foreign collection.
    deque<DocumentSource::GetNextResult> mockForeignContents{Document{{"_id", 0}}};
    expCtx->setMongoProcessInterface(std::make_unique<MockMongoInterface>(mockForeignContents));

    ASSERT_THROWS_CODE(lookupChangeStage->getNext(), AssertionException, 40579);
}

TEST_F(DocumentSourceChangeStreamAddPostImageTest, ShouldPassIfDatabaseMatchesOnCollectionlessNss) {
    auto expCtx = getExpCtx();

    expCtx->setNamespaceString(NamespaceString::makeCollectionlessAggregateNSS(
        DatabaseName::createDatabaseName_forTest(boost::none, "test")));

    // Set up the lookup change post image document source.
    auto lookupChangeDS = DocumentSourceChangeStreamAddPostImage::create(expCtx, getSpec());

    // Mock out the foreign collection.
    deque<DocumentSource::GetNextResult> mockForeignContents{Document{{"_id", 0}}};
    expCtx->setMongoProcessInterface(std::make_unique<MockMongoInterface>(mockForeignContents));

    auto mockLocalStage = exec::agg::MockStage::createForTest(
        Document{{"_id", makeResumeToken(0)},
                 {"documentKey", Document{{"_id", 0}}},
                 {"operationType", "update"_sd},
                 {"ns",
                  Document{{"db", expCtx->getNamespaceString().db_forTest()},
                           {"coll", "irrelevant"_sd}}}},
        expCtx);

    auto lookupChangeStage = exec::agg::buildStage(lookupChangeDS);
    lookupChangeStage->setSource(mockLocalStage.get());

    auto next = lookupChangeStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(),
                       (Document{{"_id", makeResumeToken(0)},
                                 {"documentKey", Document{{"_id", 0}}},
                                 {"operationType", "update"_sd},
                                 {"ns",
                                  Document{{"db", expCtx->getNamespaceString().db_forTest()},
                                           {"coll", "irrelevant"_sd}}},
                                 {"fullDocument", Document{{"_id", 0}}}}));
}

TEST_F(DocumentSourceChangeStreamAddPostImageTest, ShouldErrorIfDocumentKeyIsNotUnique) {
    auto expCtx = getExpCtx();

    // Set up the lookup change post image document source.
    auto lookupChangeDS = DocumentSourceChangeStreamAddPostImage::create(expCtx, getSpec());

    // Mock its input with an update document.
    auto mockLocalStage = exec::agg::MockStage::createForTest(
        Document{{"_id", makeResumeToken(0)},
                 {"documentKey", Document{{"_id", 0}}},
                 {"operationType", "update"_sd},
                 {"ns",
                  Document{{"db", expCtx->getNamespaceString().db_forTest()},
                           {"coll", expCtx->getNamespaceString().coll()}}}},
        expCtx);

    auto lookupChangeStage = exec::agg::buildStage(lookupChangeDS);
    lookupChangeStage->setSource(mockLocalStage.get());

    // Mock out the foreign collection to have two documents with the same document key.
    deque<DocumentSource::GetNextResult> foreignCollection = {Document{{"_id", 0}},
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
        {Document{{"_id", makeResumeToken(0)},
                  {"documentKey", Document{{"_id", 0}}},
                  {"operationType", "insert"_sd},
                  {"ns",
                   Document{{"db", expCtx->getNamespaceString().db_forTest()},
                            {"coll", expCtx->getNamespaceString().coll()}}},
                  {"fullDocument", Document{{"_id", 0}}}},
         DocumentSource::GetNextResult::makePauseExecution(),
         Document{{"_id", makeResumeToken(1)},
                  {"documentKey", Document{{"_id", 1}}},
                  {"operationType", "update"_sd},
                  {"ns",
                   Document{{"db", expCtx->getNamespaceString().db_forTest()},
                            {"coll", expCtx->getNamespaceString().coll()}}}},
         DocumentSource::GetNextResult::makePauseExecution()},
        expCtx);

    auto lookupChangeStage = exec::agg::buildStage(lookupChangeDS);
    lookupChangeStage->setSource(mockLocalStage.get());

    // Mock out the foreign collection.
    deque<DocumentSource::GetNextResult> mockForeignContents{Document{{"_id", 0}},
                                                             Document{{"_id", 1}}};
    getExpCtx()->setMongoProcessInterface(
        std::make_unique<MockMongoInterface>(std::move(mockForeignContents)));

    auto next = lookupChangeStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(),
                       (Document{{"_id", makeResumeToken(0)},
                                 {"documentKey", Document{{"_id", 0}}},
                                 {"operationType", "insert"_sd},
                                 {"ns",
                                  Document{{"db", expCtx->getNamespaceString().db_forTest()},
                                           {"coll", expCtx->getNamespaceString().coll()}}},
                                 {"fullDocument", Document{{"_id", 0}}}}));

    ASSERT_TRUE(lookupChangeStage->getNext().isPaused());

    next = lookupChangeStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(),
                       (Document{{"_id", makeResumeToken(1)},
                                 {"documentKey", Document{{"_id", 1}}},
                                 {"operationType", "update"_sd},
                                 {"ns",
                                  Document{{"db", expCtx->getNamespaceString().db_forTest()},
                                           {"coll", expCtx->getNamespaceString().coll()}}},
                                 {"fullDocument", Document{{"_id", 1}}}}));

    ASSERT_TRUE(lookupChangeStage->getNext().isPaused());

    ASSERT_TRUE(lookupChangeStage->getNext().isEOF());
    ASSERT_TRUE(lookupChangeStage->getNext().isEOF());
}

}  // namespace
}  // namespace mongo
