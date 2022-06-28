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

#include "mongo/platform/basic.h"

#include <boost/intrusive_ptr.hpp>
#include <deque>
#include <vector>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_change_stream_add_post_image.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/process_interface/stub_lookup_single_document_process_interface.h"
#include "mongo/idl/server_parameter_test_util.h"

namespace mongo {
namespace {
using boost::intrusive_ptr;
using std::deque;
using std::vector;

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
    auto spec = BSON("$changeStream: " << BSON("fullDocument"
                                               << "banana"));
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

    ASSERT_VALUE_EQ(stage->serialize({ExplainOptions::Verbosity::kQueryPlanner}), expectedOutput);
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

    // Set up the lookup change post image stage.
    auto lookupChangeStage = DocumentSourceChangeStreamAddPostImage::create(expCtx, getSpec());

    // Mock its input with a document without a "documentKey" field.
    auto mockLocalSource = DocumentSourceMock::createForTest(
        Document{{"_id", makeResumeToken(0)},
                 {"operationType", "update"_sd},
                 {"fullDocument", Document{{"_id", 0}}},
                 {"ns", Document{{"db", expCtx->ns.db()}, {"coll", expCtx->ns.coll()}}}},
        expCtx);

    lookupChangeStage->setSource(mockLocalSource.get());

    // Mock out the foreign collection.
    getExpCtx()->mongoProcessInterface =
        std::make_unique<MockMongoInterface>(deque<DocumentSource::GetNextResult>{});

    ASSERT_THROWS_CODE(lookupChangeStage->getNext(), AssertionException, 40578);
}

TEST_F(DocumentSourceChangeStreamAddPostImageTest, ShouldErrorIfMissingOperationType) {
    auto expCtx = getExpCtx();

    // Set up the lookup change post image stage.
    auto lookupChangeStage = DocumentSourceChangeStreamAddPostImage::create(expCtx, getSpec());

    // Mock its input with a document without a "ns" field.
    auto mockLocalSource = DocumentSourceMock::createForTest(
        Document{{"_id", makeResumeToken(0)},
                 {"documentKey", Document{{"_id", 0}}},
                 {"fullDocument", Document{{"_id", 0}}},
                 {"ns", Document{{"db", expCtx->ns.db()}, {"coll", expCtx->ns.coll()}}}},
        expCtx);

    lookupChangeStage->setSource(mockLocalSource.get());

    // Mock out the foreign collection.
    getExpCtx()->mongoProcessInterface =
        std::make_unique<MockMongoInterface>(deque<DocumentSource::GetNextResult>{});

    ASSERT_THROWS_CODE(lookupChangeStage->getNext(), AssertionException, 40578);
}

TEST_F(DocumentSourceChangeStreamAddPostImageTest, ShouldErrorIfMissingNamespace) {
    auto expCtx = getExpCtx();

    // Set up the lookup change post image stage.
    auto lookupChangeStage = DocumentSourceChangeStreamAddPostImage::create(expCtx, getSpec());

    // Mock its input with a document without a "ns" field.
    auto mockLocalSource = DocumentSourceMock::createForTest(
        Document{
            {"_id", makeResumeToken(0)},
            {"documentKey", Document{{"_id", 0}}},
            {"operationType", "update"_sd},
        },
        expCtx);

    lookupChangeStage->setSource(mockLocalSource.get());

    // Mock out the foreign collection.
    getExpCtx()->mongoProcessInterface =
        std::make_unique<MockMongoInterface>(deque<DocumentSource::GetNextResult>{});

    ASSERT_THROWS_CODE(lookupChangeStage->getNext(), AssertionException, 40578);
}

TEST_F(DocumentSourceChangeStreamAddPostImageTest, ShouldErrorIfNsFieldHasWrongType) {
    auto expCtx = getExpCtx();

    // Set up the lookup change post image stage.
    auto lookupChangeStage = DocumentSourceChangeStreamAddPostImage::create(expCtx, getSpec());

    // Mock its input with a document without a "ns" field.
    auto mockLocalSource =
        DocumentSourceMock::createForTest(Document{{"_id", makeResumeToken(0)},
                                                   {"documentKey", Document{{"_id", 0}}},
                                                   {"operationType", "update"_sd},
                                                   {"ns", 4}},
                                          expCtx);

    lookupChangeStage->setSource(mockLocalSource.get());

    // Mock out the foreign collection.
    getExpCtx()->mongoProcessInterface =
        std::make_unique<MockMongoInterface>(deque<DocumentSource::GetNextResult>{});

    ASSERT_THROWS_CODE(lookupChangeStage->getNext(), AssertionException, 40578);
}

TEST_F(DocumentSourceChangeStreamAddPostImageTest, ShouldErrorIfNsFieldDoesNotMatchPipeline) {
    auto expCtx = getExpCtx();

    // Set up the lookup change post image stage.
    auto lookupChangeStage = DocumentSourceChangeStreamAddPostImage::create(expCtx, getSpec());

    // Mock its input with a document without a "ns" field.
    auto mockLocalSource = DocumentSourceMock::createForTest(
        Document{{"_id", makeResumeToken(0)},
                 {"documentKey", Document{{"_id", 0}}},
                 {"operationType", "update"_sd},
                 {"ns", Document{{"db", "DIFFERENT"_sd}, {"coll", expCtx->ns.coll()}}}},
        expCtx);

    lookupChangeStage->setSource(mockLocalSource.get());

    // Mock out the foreign collection.
    getExpCtx()->mongoProcessInterface =
        std::make_unique<MockMongoInterface>(deque<DocumentSource::GetNextResult>{});

    ASSERT_THROWS_CODE(lookupChangeStage->getNext(), AssertionException, 40579);
}

TEST_F(DocumentSourceChangeStreamAddPostImageTest,
       ShouldErrorIfDatabaseMismatchOnCollectionlessNss) {
    auto expCtx = getExpCtx();

    expCtx->ns = NamespaceString::makeCollectionlessAggregateNSS(DatabaseName(boost::none, "test"));

    // Set up the lookup change post image stage.
    auto lookupChangeStage = DocumentSourceChangeStreamAddPostImage::create(expCtx, getSpec());

    // Mock its input with a document without a "ns" field.
    auto mockLocalSource = DocumentSourceMock::createForTest(
        Document{{"_id", makeResumeToken(0)},
                 {"documentKey", Document{{"_id", 0}}},
                 {"operationType", "update"_sd},
                 {"ns", Document{{"db", "DIFFERENT"_sd}, {"coll", "irrelevant"_sd}}}},
        expCtx);

    lookupChangeStage->setSource(mockLocalSource.get());

    // Mock out the foreign collection.
    deque<DocumentSource::GetNextResult> mockForeignContents{Document{{"_id", 0}}};
    expCtx->mongoProcessInterface = std::make_unique<MockMongoInterface>(mockForeignContents);

    ASSERT_THROWS_CODE(lookupChangeStage->getNext(), AssertionException, 40579);
}

TEST_F(DocumentSourceChangeStreamAddPostImageTest, ShouldPassIfDatabaseMatchesOnCollectionlessNss) {
    auto expCtx = getExpCtx();

    expCtx->ns = NamespaceString::makeCollectionlessAggregateNSS(DatabaseName(boost::none, "test"));

    // Set up the lookup change post image stage.
    auto lookupChangeStage = DocumentSourceChangeStreamAddPostImage::create(expCtx, getSpec());

    // Mock out the foreign collection.
    deque<DocumentSource::GetNextResult> mockForeignContents{Document{{"_id", 0}}};
    expCtx->mongoProcessInterface = std::make_unique<MockMongoInterface>(mockForeignContents);

    auto mockLocalSource = DocumentSourceMock::createForTest(
        Document{{"_id", makeResumeToken(0)},
                 {"documentKey", Document{{"_id", 0}}},
                 {"operationType", "update"_sd},
                 {"ns", Document{{"db", expCtx->ns.db()}, {"coll", "irrelevant"_sd}}}},
        expCtx);

    lookupChangeStage->setSource(mockLocalSource.get());

    auto next = lookupChangeStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(
        next.releaseDocument(),
        (Document{{"_id", makeResumeToken(0)},
                  {"documentKey", Document{{"_id", 0}}},
                  {"operationType", "update"_sd},
                  {"ns", Document{{"db", expCtx->ns.db()}, {"coll", "irrelevant"_sd}}},
                  {"fullDocument", Document{{"_id", 0}}}}));
}

TEST_F(DocumentSourceChangeStreamAddPostImageTest, ShouldErrorIfDocumentKeyIsNotUnique) {
    auto expCtx = getExpCtx();

    // Set up the lookup change post image stage.
    auto lookupChangeStage = DocumentSourceChangeStreamAddPostImage::create(expCtx, getSpec());

    // Mock its input with an update document.
    auto mockLocalSource = DocumentSourceMock::createForTest(
        Document{{"_id", makeResumeToken(0)},
                 {"documentKey", Document{{"_id", 0}}},
                 {"operationType", "update"_sd},
                 {"ns", Document{{"db", expCtx->ns.db()}, {"coll", expCtx->ns.coll()}}}},
        expCtx);

    lookupChangeStage->setSource(mockLocalSource.get());

    // Mock out the foreign collection to have two documents with the same document key.
    deque<DocumentSource::GetNextResult> foreignCollection = {Document{{"_id", 0}},
                                                              Document{{"_id", 0}}};
    getExpCtx()->mongoProcessInterface =
        std::make_unique<MockMongoInterface>(std::move(foreignCollection));

    ASSERT_THROWS_CODE(
        lookupChangeStage->getNext(), AssertionException, ErrorCodes::TooManyMatchingDocuments);
}

TEST_F(DocumentSourceChangeStreamAddPostImageTest, ShouldPropagatePauses) {
    auto expCtx = getExpCtx();

    // Set up the lookup change post image stage.
    auto lookupChangeStage = DocumentSourceChangeStreamAddPostImage::create(expCtx, getSpec());

    // Mock its input, pausing every other result.
    auto mockLocalSource = DocumentSourceMock::createForTest(
        {Document{{"_id", makeResumeToken(0)},
                  {"documentKey", Document{{"_id", 0}}},
                  {"operationType", "insert"_sd},
                  {"ns", Document{{"db", expCtx->ns.db()}, {"coll", expCtx->ns.coll()}}},
                  {"fullDocument", Document{{"_id", 0}}}},
         DocumentSource::GetNextResult::makePauseExecution(),
         Document{{"_id", makeResumeToken(1)},
                  {"documentKey", Document{{"_id", 1}}},
                  {"operationType", "update"_sd},
                  {"ns", Document{{"db", expCtx->ns.db()}, {"coll", expCtx->ns.coll()}}}},
         DocumentSource::GetNextResult::makePauseExecution()},
        expCtx);

    lookupChangeStage->setSource(mockLocalSource.get());

    // Mock out the foreign collection.
    deque<DocumentSource::GetNextResult> mockForeignContents{Document{{"_id", 0}},
                                                             Document{{"_id", 1}}};
    getExpCtx()->mongoProcessInterface =
        std::make_unique<MockMongoInterface>(std::move(mockForeignContents));

    auto next = lookupChangeStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(
        next.releaseDocument(),
        (Document{{"_id", makeResumeToken(0)},
                  {"documentKey", Document{{"_id", 0}}},
                  {"operationType", "insert"_sd},
                  {"ns", Document{{"db", expCtx->ns.db()}, {"coll", expCtx->ns.coll()}}},
                  {"fullDocument", Document{{"_id", 0}}}}));

    ASSERT_TRUE(lookupChangeStage->getNext().isPaused());

    next = lookupChangeStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(
        next.releaseDocument(),
        (Document{{"_id", makeResumeToken(1)},
                  {"documentKey", Document{{"_id", 1}}},
                  {"operationType", "update"_sd},
                  {"ns", Document{{"db", expCtx->ns.db()}, {"coll", expCtx->ns.coll()}}},
                  {"fullDocument", Document{{"_id", 1}}}}));

    ASSERT_TRUE(lookupChangeStage->getNext().isPaused());

    ASSERT_TRUE(lookupChangeStage->getNext().isEOF());
    ASSERT_TRUE(lookupChangeStage->getNext().isEOF());
}

}  // namespace
}  // namespace mongo
