/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <boost/intrusive_ptr.hpp>
#include <deque>
#include <vector>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_lookup_change_post_image.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/stub_mongod_interface.h"
#include "mongo/db/pipeline/value.h"

namespace mongo {
namespace {
using boost::intrusive_ptr;
using std::deque;
using std::vector;

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
using DocumentSourceLookupChangePostImageTest = AggregationContextFixture;

/**
 * A mock MongodInterface which allows mocking a foreign pipeline.
 */
class MockMongodInterface final : public StubMongodInterface {
public:
    MockMongodInterface(deque<DocumentSource::GetNextResult> mockResults)
        : _mockResults(std::move(mockResults)) {}

    bool isSharded(const NamespaceString& ns) final {
        return false;
    }

    StatusWith<std::unique_ptr<Pipeline, Pipeline::Deleter>> makePipeline(
        const std::vector<BSONObj>& rawPipeline,
        const boost::intrusive_ptr<ExpressionContext>& expCtx) final {
        auto pipeline = Pipeline::parse(rawPipeline, expCtx);
        if (!pipeline.isOK()) {
            return pipeline.getStatus();
        }

        pipeline.getValue()->addInitialSource(DocumentSourceMock::create(_mockResults));
        pipeline.getValue()->optimizePipeline();

        return pipeline;
    }

private:
    deque<DocumentSource::GetNextResult> _mockResults;
};

TEST_F(DocumentSourceLookupChangePostImageTest, ShouldErrorIfMissingDocumentKeyOnUpdate) {
    auto expCtx = getExpCtx();

    // Set up the $lookup stage.
    auto lookupChangeStage = DocumentSourceLookupChangePostImage::create(expCtx);

    // Mock its input with a document without a "documentKey" field.
    auto mockLocalSource = DocumentSourceMock::create(
        Document{{"operationType", "update"_sd},
                 {"fullDocument", Document{{"_id", 0}}},
                 {"ns", Document{{"db", expCtx->ns.db()}, {"coll", expCtx->ns.coll()}}}});

    lookupChangeStage->setSource(mockLocalSource.get());

    // Mock out the foreign collection.
    lookupChangeStage->injectMongodInterface(
        std::make_shared<MockMongodInterface>(deque<DocumentSource::GetNextResult>{}));

    ASSERT_THROWS_CODE(lookupChangeStage->getNext(), AssertionException, 40578);
}

TEST_F(DocumentSourceLookupChangePostImageTest, ShouldErrorIfMissingOperationType) {
    auto expCtx = getExpCtx();

    // Set up the $lookup stage.
    auto lookupChangeStage = DocumentSourceLookupChangePostImage::create(expCtx);

    // Mock its input with a document without a "ns" field.
    auto mockLocalSource = DocumentSourceMock::create(
        Document{{"documentKey", Document{{"_id", 0}}},
                 {"fullDocument", Document{{"_id", 0}}},
                 {"ns", Document{{"db", expCtx->ns.db()}, {"coll", expCtx->ns.coll()}}}});

    lookupChangeStage->setSource(mockLocalSource.get());

    // Mock out the foreign collection.
    lookupChangeStage->injectMongodInterface(
        std::make_shared<MockMongodInterface>(deque<DocumentSource::GetNextResult>{}));

    ASSERT_THROWS_CODE(lookupChangeStage->getNext(), AssertionException, 40578);
}

TEST_F(DocumentSourceLookupChangePostImageTest, ShouldErrorIfMissingNamespace) {
    auto expCtx = getExpCtx();

    // Set up the $lookup stage.
    auto lookupChangeStage = DocumentSourceLookupChangePostImage::create(expCtx);

    // Mock its input with a document without a "ns" field.
    auto mockLocalSource = DocumentSourceMock::create(Document{
        {"documentKey", Document{{"_id", 0}}}, {"operationType", "update"_sd},
    });

    lookupChangeStage->setSource(mockLocalSource.get());

    // Mock out the foreign collection.
    lookupChangeStage->injectMongodInterface(
        std::make_shared<MockMongodInterface>(deque<DocumentSource::GetNextResult>{}));

    ASSERT_THROWS_CODE(lookupChangeStage->getNext(), AssertionException, 40578);
}

TEST_F(DocumentSourceLookupChangePostImageTest, ShouldErrorIfNsFieldHasWrongType) {
    auto expCtx = getExpCtx();

    // Set up the $lookup stage.
    auto lookupChangeStage = DocumentSourceLookupChangePostImage::create(expCtx);

    // Mock its input with a document without a "ns" field.
    auto mockLocalSource = DocumentSourceMock::create(
        Document{{"documentKey", Document{{"_id", 0}}}, {"operationType", "update"_sd}, {"ns", 4}});

    lookupChangeStage->setSource(mockLocalSource.get());

    // Mock out the foreign collection.
    lookupChangeStage->injectMongodInterface(
        std::make_shared<MockMongodInterface>(deque<DocumentSource::GetNextResult>{}));

    ASSERT_THROWS_CODE(lookupChangeStage->getNext(), AssertionException, 40578);
}

TEST_F(DocumentSourceLookupChangePostImageTest, ShouldErrorIfNsFieldDoesNotMatchPipeline) {
    auto expCtx = getExpCtx();

    // Set up the $lookup stage.
    auto lookupChangeStage = DocumentSourceLookupChangePostImage::create(expCtx);

    // Mock its input with a document without a "ns" field.
    auto mockLocalSource = DocumentSourceMock::create(
        Document{{"documentKey", Document{{"_id", 0}}},
                 {"operationType", "update"_sd},
                 {"ns", Document{{"db", "DIFFERENT"_sd}, {"coll", expCtx->ns.coll()}}}});

    lookupChangeStage->setSource(mockLocalSource.get());

    // Mock out the foreign collection.
    lookupChangeStage->injectMongodInterface(
        std::make_shared<MockMongodInterface>(deque<DocumentSource::GetNextResult>{}));

    ASSERT_THROWS_CODE(lookupChangeStage->getNext(), AssertionException, 40579);
}

TEST_F(DocumentSourceLookupChangePostImageTest, ShouldErrorIfDocumentKeyIsNotUnique) {
    auto expCtx = getExpCtx();

    // Set up the $lookup stage.
    auto lookupChangeStage = DocumentSourceLookupChangePostImage::create(expCtx);

    // Mock its input with an update document.
    auto mockLocalSource = DocumentSourceMock::create(
        Document{{"documentKey", Document{{"_id", 0}}},
                 {"operationType", "update"_sd},
                 {"ns", Document{{"db", expCtx->ns.db()}, {"coll", expCtx->ns.coll()}}}});

    lookupChangeStage->setSource(mockLocalSource.get());

    // Mock out the foreign collection to have two documents with the same document key.
    deque<DocumentSource::GetNextResult> foreignCollection = {Document{{"_id", 0}},
                                                              Document{{"_id", 0}}};
    lookupChangeStage->injectMongodInterface(
        std::make_shared<MockMongodInterface>(std::move(foreignCollection)));

    ASSERT_THROWS_CODE(lookupChangeStage->getNext(), AssertionException, 40580);
}

TEST_F(DocumentSourceLookupChangePostImageTest, ShouldPropagatePauses) {
    auto expCtx = getExpCtx();

    // Set up the $lookup stage.
    auto lookupChangeStage = DocumentSourceLookupChangePostImage::create(expCtx);

    // Mock its input, pausing every other result.
    auto mockLocalSource = DocumentSourceMock::create(
        {Document{{"documentKey", Document{{"_id", 0}}},
                  {"operationType", "insert"_sd},
                  {"ns", Document{{"db", expCtx->ns.db()}, {"coll", expCtx->ns.coll()}}},
                  {"fullDocument", Document{{"_id", 0}}}},
         DocumentSource::GetNextResult::makePauseExecution(),
         Document{{"documentKey", Document{{"_id", 1}}},
                  {"operationType", "update"_sd},
                  {"ns", Document{{"db", expCtx->ns.db()}, {"coll", expCtx->ns.coll()}}}},
         DocumentSource::GetNextResult::makePauseExecution()});

    lookupChangeStage->setSource(mockLocalSource.get());

    // Mock out the foreign collection.
    deque<DocumentSource::GetNextResult> mockForeignContents{Document{{"_id", 0}},
                                                             Document{{"_id", 1}}};
    lookupChangeStage->injectMongodInterface(
        std::make_shared<MockMongodInterface>(std::move(mockForeignContents)));

    auto next = lookupChangeStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(
        next.releaseDocument(),
        (Document{{"documentKey", Document{{"_id", 0}}},
                  {"operationType", "insert"_sd},
                  {"ns", Document{{"db", expCtx->ns.db()}, {"coll", expCtx->ns.coll()}}},
                  {"fullDocument", Document{{"_id", 0}}}}));

    ASSERT_TRUE(lookupChangeStage->getNext().isPaused());

    next = lookupChangeStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(
        next.releaseDocument(),
        (Document{{"documentKey", Document{{"_id", 1}}},
                  {"operationType", "update"_sd},
                  {"ns", Document{{"db", expCtx->ns.db()}, {"coll", expCtx->ns.coll()}}},
                  {"fullDocument", Document{{"_id", 1}}}}));

    ASSERT_TRUE(lookupChangeStage->getNext().isPaused());

    ASSERT_TRUE(lookupChangeStage->getNext().isEOF());
    ASSERT_TRUE(lookupChangeStage->getNext().isEOF());
}

}  // namespace
}  // namespace mongo
