/**
 *    Copyright (C) 2016 MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include <algorithm>
#include <deque>

#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source_graph_lookup.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/pipeline/stub_mongod_interface.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

// Crutch.
bool isMongos() {
    return false;
}

namespace {

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
using DocumentSourceGraphLookUpTest = AggregationContextFixture;

//
// Evaluation.
//

/**
 * A MongodInterface use for testing that supports making pipelines with an initial
 * DocumentSourceMock source.
 */
class MockMongodImplementation final : public StubMongodInterface {
public:
    MockMongodImplementation(std::deque<DocumentSource::GetNextResult> results)
        : _results(std::move(results)) {}

    StatusWith<boost::intrusive_ptr<Pipeline>> makePipeline(
        const std::vector<BSONObj>& rawPipeline,
        const boost::intrusive_ptr<ExpressionContext>& expCtx) final {
        auto pipeline = Pipeline::parse(rawPipeline, expCtx);
        if (!pipeline.isOK()) {
            return pipeline.getStatus();
        }

        pipeline.getValue()->addInitialSource(DocumentSourceMock::create(_results));
        pipeline.getValue()->optimizePipeline();

        return pipeline;
    }

private:
    std::deque<DocumentSource::GetNextResult> _results;
};

TEST_F(DocumentSourceGraphLookUpTest,
       ShouldErrorWhenDoingInitialMatchIfDocumentInFromCollectionIsMissingId) {
    auto expCtx = getExpCtx();

    std::deque<DocumentSource::GetNextResult> inputs{Document{{"_id", 0}}};
    auto inputMock = DocumentSourceMock::create(std::move(inputs));

    std::deque<DocumentSource::GetNextResult> fromContents{Document{{"to", 0}}};

    NamespaceString fromNs("test", "graph_lookup");
    expCtx->setResolvedNamespace(fromNs, {fromNs, std::vector<BSONObj>{}});
    auto graphLookupStage =
        DocumentSourceGraphLookUp::create(expCtx,
                                          fromNs,
                                          "results",
                                          "from",
                                          "to",
                                          ExpressionFieldPath::create(expCtx, "_id"),
                                          boost::none,
                                          boost::none,
                                          boost::none,
                                          boost::none);
    graphLookupStage->setSource(inputMock.get());
    graphLookupStage->injectMongodInterface(
        std::make_shared<MockMongodImplementation>(std::move(fromContents)));

    ASSERT_THROWS_CODE(graphLookupStage->getNext(), UserException, 40271);
}

TEST_F(DocumentSourceGraphLookUpTest,
       ShouldErrorWhenExploringGraphIfDocumentInFromCollectionIsMissingId) {
    auto expCtx = getExpCtx();

    std::deque<DocumentSource::GetNextResult> inputs{Document{{"_id", 0}}};
    auto inputMock = DocumentSourceMock::create(std::move(inputs));

    std::deque<DocumentSource::GetNextResult> fromContents{
        Document{{"_id", "a"}, {"to", 0}, {"from", 1}}, Document{{"to", 1}}};

    NamespaceString fromNs("test", "graph_lookup");
    expCtx->setResolvedNamespace(fromNs, {fromNs, std::vector<BSONObj>{}});
    auto graphLookupStage =
        DocumentSourceGraphLookUp::create(expCtx,
                                          fromNs,
                                          "results",
                                          "from",
                                          "to",
                                          ExpressionFieldPath::create(expCtx, "_id"),
                                          boost::none,
                                          boost::none,
                                          boost::none,
                                          boost::none);
    graphLookupStage->setSource(inputMock.get());
    graphLookupStage->injectMongodInterface(
        std::make_shared<MockMongodImplementation>(std::move(fromContents)));

    ASSERT_THROWS_CODE(graphLookupStage->getNext(), UserException, 40271);
}

TEST_F(DocumentSourceGraphLookUpTest,
       ShouldErrorWhenHandlingUnwindIfDocumentInFromCollectionIsMissingId) {
    auto expCtx = getExpCtx();

    std::deque<DocumentSource::GetNextResult> inputs{Document{{"_id", 0}}};
    auto inputMock = DocumentSourceMock::create(std::move(inputs));

    std::deque<DocumentSource::GetNextResult> fromContents{Document{{"to", 0}}};

    NamespaceString fromNs("test", "graph_lookup");
    expCtx->setResolvedNamespace(fromNs, {fromNs, std::vector<BSONObj>{}});
    auto unwindStage = DocumentSourceUnwind::create(expCtx, "results", false, boost::none);
    auto graphLookupStage =
        DocumentSourceGraphLookUp::create(expCtx,
                                          fromNs,
                                          "results",
                                          "from",
                                          "to",
                                          ExpressionFieldPath::create(expCtx, "_id"),
                                          boost::none,
                                          boost::none,
                                          boost::none,
                                          unwindStage);
    graphLookupStage->injectMongodInterface(
        std::make_shared<MockMongodImplementation>(std::move(fromContents)));
    graphLookupStage->setSource(inputMock.get());

    ASSERT_THROWS_CODE(graphLookupStage->getNext(), UserException, 40271);
}

bool arrayContains(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                   const std::vector<Value>& arr,
                   const Value& elem) {
    auto result = std::find_if(arr.begin(), arr.end(), [&expCtx, &elem](const Value& other) {
        return expCtx->getValueComparator().evaluate(elem == other);
    });
    return result != arr.end();
}

TEST_F(DocumentSourceGraphLookUpTest,
       ShouldTraverseSubgraphIfIdOfDocumentsInFromCollectionAreNonUnique) {
    auto expCtx = getExpCtx();

    std::deque<DocumentSource::GetNextResult> inputs{Document{{"_id", 0}}};
    auto inputMock = DocumentSourceMock::create(std::move(inputs));

    Document to0from1{{"_id", "a"}, {"to", 0}, {"from", 1}};
    Document to0from2{{"_id", "a"}, {"to", 0}, {"from", 2}};
    Document to1{{"_id", "b"}, {"to", 1}};
    Document to2{{"_id", "c"}, {"to", 2}};
    std::deque<DocumentSource::GetNextResult> fromContents{
        Document(to1), Document(to2), Document(to0from1), Document(to0from2)};

    NamespaceString fromNs("test", "graph_lookup");
    expCtx->setResolvedNamespace(fromNs, {fromNs, std::vector<BSONObj>{}});
    auto graphLookupStage =
        DocumentSourceGraphLookUp::create(expCtx,
                                          fromNs,
                                          "results",
                                          "from",
                                          "to",
                                          ExpressionFieldPath::create(expCtx, "_id"),
                                          boost::none,
                                          boost::none,
                                          boost::none,
                                          boost::none);
    graphLookupStage->setSource(inputMock.get());
    graphLookupStage->injectMongodInterface(
        std::make_shared<MockMongodImplementation>(std::move(fromContents)));
    graphLookupStage->setSource(inputMock.get());

    auto next = graphLookupStage->getNext();
    ASSERT_TRUE(next.isAdvanced());

    ASSERT_EQ(2U, next.getDocument().size());
    ASSERT_VALUE_EQ(Value(0), next.getDocument().getField("_id"));

    auto resultsValue = next.getDocument().getField("results");
    ASSERT(resultsValue.isArray());
    auto resultsArray = resultsValue.getArray();

    // Since 'to0from1' and 'to0from2' have the same _id, we should end up only exploring the path
    // through one of them.
    if (arrayContains(expCtx, resultsArray, Value(to0from1))) {
        // If 'to0from1' was returned, then we should see 'to1' and nothing else.
        ASSERT(arrayContains(expCtx, resultsArray, Value(to1)));
        ASSERT_EQ(2U, resultsArray.size());

        next = graphLookupStage->getNext();
        ASSERT(next.isEOF());
    } else if (arrayContains(expCtx, resultsArray, Value(to0from2))) {
        // If 'to0from2' was returned, then we should see 'to2' and nothing else.
        ASSERT(arrayContains(expCtx, resultsArray, Value(to2)));
        ASSERT_EQ(2U, resultsArray.size());

        next = graphLookupStage->getNext();
        ASSERT(next.isEOF());
    } else {
        FAIL(str::stream() << "Expected either [ " << to0from1.toString() << " ] or [ "
                           << to0from2.toString()
                           << " ] but found [ "
                           << next.getDocument().toString()
                           << " ]");
    }
}

TEST_F(DocumentSourceGraphLookUpTest, ShouldPropagatePauses) {
    auto expCtx = getExpCtx();

    auto inputMock =
        DocumentSourceMock::create({Document{{"startPoint", 0}},
                                    DocumentSource::GetNextResult::makePauseExecution(),
                                    Document{{"startPoint", 0}},
                                    DocumentSource::GetNextResult::makePauseExecution()});

    std::deque<DocumentSource::GetNextResult> fromContents{
        Document{{"_id", "a"}, {"to", 0}, {"from", 1}}, Document{{"_id", "b"}, {"to", 1}}};

    NamespaceString fromNs("test", "foreign");
    expCtx->setResolvedNamespace(fromNs, {fromNs, std::vector<BSONObj>{}});
    auto graphLookupStage =
        DocumentSourceGraphLookUp::create(expCtx,
                                          fromNs,
                                          "results",
                                          "from",
                                          "to",
                                          ExpressionFieldPath::create(expCtx, "startPoint"),
                                          boost::none,
                                          boost::none,
                                          boost::none,
                                          boost::none);

    graphLookupStage->setSource(inputMock.get());

    graphLookupStage->injectMongodInterface(
        std::make_shared<MockMongodImplementation>(std::move(fromContents)));

    auto next = graphLookupStage->getNext();
    ASSERT_TRUE(next.isAdvanced());

    // We expect {startPoint: 0, results: [{_id: "a", to: 0, from: 1}, {_id: "b", to: 1}]}, but the
    // 'results' array can be in any order. So we use arrayContains to assert it has the right
    // contents.
    auto result = next.releaseDocument();
    ASSERT_VALUE_EQ(result["startPoint"], Value(0));
    ASSERT_EQ(result["results"].getType(), BSONType::Array);
    ASSERT_EQ(result["results"].getArray().size(), 2UL);
    ASSERT_TRUE(arrayContains(expCtx,
                              result["results"].getArray(),
                              Value(Document{{"_id", "a"}, {"to", 0}, {"from", 1}})));
    ASSERT_TRUE(arrayContains(
        expCtx, result["results"].getArray(), Value(Document{{"_id", "b"}, {"to", 1}})));

    ASSERT_TRUE(graphLookupStage->getNext().isPaused());

    next = graphLookupStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    result = next.releaseDocument();
    ASSERT_VALUE_EQ(result["startPoint"], Value(0));
    ASSERT_EQ(result["results"].getType(), BSONType::Array);
    ASSERT_EQ(result["results"].getArray().size(), 2UL);
    ASSERT_TRUE(arrayContains(expCtx,
                              result["results"].getArray(),
                              Value(Document{{"_id", "a"}, {"to", 0}, {"from", 1}})));
    ASSERT_TRUE(arrayContains(
        expCtx, result["results"].getArray(), Value(Document{{"_id", "b"}, {"to", 1}})));

    ASSERT_TRUE(graphLookupStage->getNext().isPaused());

    ASSERT_TRUE(graphLookupStage->getNext().isEOF());
    ASSERT_TRUE(graphLookupStage->getNext().isEOF());
}

TEST_F(DocumentSourceGraphLookUpTest, ShouldPropagatePausesWhileUnwinding) {
    auto expCtx = getExpCtx();

    // Set up the $graphLookup stage
    auto inputMock =
        DocumentSourceMock::create({Document{{"startPoint", 0}},
                                    DocumentSource::GetNextResult::makePauseExecution(),
                                    Document{{"startPoint", 0}},
                                    DocumentSource::GetNextResult::makePauseExecution()});

    std::deque<DocumentSource::GetNextResult> fromContents{
        Document{{"_id", "a"}, {"to", 0}, {"from", 1}}, Document{{"_id", "b"}, {"to", 1}}};

    NamespaceString fromNs("test", "foreign");
    expCtx->setResolvedNamespace(fromNs, {fromNs, std::vector<BSONObj>{}});

    const bool preserveNullAndEmptyArrays = false;
    const boost::optional<std::string> includeArrayIndex = boost::none;
    auto unwindStage = DocumentSourceUnwind::create(
        expCtx, "results", preserveNullAndEmptyArrays, includeArrayIndex);

    auto graphLookupStage =
        DocumentSourceGraphLookUp::create(expCtx,
                                          fromNs,
                                          "results",
                                          "from",
                                          "to",
                                          ExpressionFieldPath::create(expCtx, "startPoint"),
                                          boost::none,
                                          boost::none,
                                          boost::none,
                                          unwindStage);

    graphLookupStage->setSource(inputMock.get());

    graphLookupStage->injectMongodInterface(
        std::make_shared<MockMongodImplementation>(std::move(fromContents)));

    // Assert it has the expected results. Note the results can be in either order.
    auto expectedA =
        Document{{"startPoint", 0}, {"results", Document{{"_id", "a"}, {"to", 0}, {"from", 1}}}};
    auto expectedB = Document{{"startPoint", 0}, {"results", Document{{"_id", "b"}, {"to", 1}}}};
    auto next = graphLookupStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    if (expCtx->getDocumentComparator().evaluate(next.getDocument() == expectedA)) {
        next = graphLookupStage->getNext();
        ASSERT_TRUE(next.isAdvanced());
        ASSERT_DOCUMENT_EQ(next.releaseDocument(), expectedB);
    } else {
        ASSERT_DOCUMENT_EQ(next.releaseDocument(), expectedB);
        next = graphLookupStage->getNext();
        ASSERT_TRUE(next.isAdvanced());
        ASSERT_DOCUMENT_EQ(next.releaseDocument(), expectedA);
    }

    ASSERT_TRUE(graphLookupStage->getNext().isPaused());

    next = graphLookupStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    if (expCtx->getDocumentComparator().evaluate(next.getDocument() == expectedA)) {
        next = graphLookupStage->getNext();
        ASSERT_TRUE(next.isAdvanced());
        ASSERT_DOCUMENT_EQ(next.releaseDocument(), expectedB);
    } else {
        ASSERT_DOCUMENT_EQ(next.releaseDocument(), expectedB);
        next = graphLookupStage->getNext();
        ASSERT_TRUE(next.isAdvanced());
        ASSERT_DOCUMENT_EQ(next.releaseDocument(), expectedA);
    }

    ASSERT_TRUE(graphLookupStage->getNext().isPaused());

    ASSERT_TRUE(graphLookupStage->getNext().isEOF());
    ASSERT_TRUE(graphLookupStage->getNext().isEOF());
}

TEST_F(DocumentSourceGraphLookUpTest, GraphLookupShouldReportAsFieldIsModified) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs("test", "foreign");
    expCtx->setResolvedNamespace(fromNs, {fromNs, std::vector<BSONObj>{}});
    auto graphLookupStage =
        DocumentSourceGraphLookUp::create(expCtx,
                                          fromNs,
                                          "results",
                                          "from",
                                          "to",
                                          ExpressionFieldPath::create(expCtx, "startPoint"),
                                          boost::none,
                                          boost::none,
                                          boost::none,
                                          boost::none);

    auto modifiedPaths = graphLookupStage->getModifiedPaths();
    ASSERT(modifiedPaths.type == DocumentSource::GetModPathsReturn::Type::kFiniteSet);
    ASSERT_EQ(1U, modifiedPaths.paths.size());
    ASSERT_EQ(1U, modifiedPaths.paths.count("results"));
}

TEST_F(DocumentSourceGraphLookUpTest, GraphLookupShouldReportFieldsModifiedByAbsorbedUnwind) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs("test", "foreign");
    expCtx->setResolvedNamespace(fromNs, {fromNs, std::vector<BSONObj>{}});
    auto unwindStage =
        DocumentSourceUnwind::create(expCtx, "results", false, std::string("arrIndex"));
    auto graphLookupStage =
        DocumentSourceGraphLookUp::create(expCtx,
                                          fromNs,
                                          "results",
                                          "from",
                                          "to",
                                          ExpressionFieldPath::create(expCtx, "startPoint"),
                                          boost::none,
                                          boost::none,
                                          boost::none,
                                          unwindStage);

    auto modifiedPaths = graphLookupStage->getModifiedPaths();
    ASSERT(modifiedPaths.type == DocumentSource::GetModPathsReturn::Type::kFiniteSet);
    ASSERT_EQ(2U, modifiedPaths.paths.size());
    ASSERT_EQ(1U, modifiedPaths.paths.count("results"));
    ASSERT_EQ(1U, modifiedPaths.paths.count("arrIndex"));
}

TEST_F(DocumentSourceGraphLookUpTest, GraphLookupWithComparisonExpressionForStartWith) {
    auto expCtx = getExpCtx();

    auto inputMock = DocumentSourceMock::create(Document({{"_id", 0}, {"a", 1}, {"b", 2}}));

    NamespaceString fromNs("test", "foreign");
    expCtx->setResolvedNamespace(fromNs, {fromNs, std::vector<BSONObj>{}});
    std::deque<DocumentSource::GetNextResult> fromContents{Document{{"_id", 0}, {"to", true}},
                                                           Document{{"_id", 1}, {"to", false}}};

    auto graphLookupStage = DocumentSourceGraphLookUp::create(
        expCtx,
        fromNs,
        "results",
        "from",
        "to",
        ExpressionCompare::create(expCtx,
                                  ExpressionCompare::GT,
                                  ExpressionFieldPath::create(expCtx, "a"),
                                  ExpressionFieldPath::create(expCtx, "b")),
        boost::none,
        boost::none,
        boost::none,
        boost::none);

    graphLookupStage->setSource(inputMock.get());
    graphLookupStage->injectMongodInterface(
        std::make_shared<MockMongodImplementation>(std::move(fromContents)));

    auto next = graphLookupStage->getNext();
    ASSERT_TRUE(next.isAdvanced());

    // The 'startWith' expression evaluates to false, so we expect the 'results' array to contain
    // just one document.
    auto actualResult = next.releaseDocument();
    Document expectedResult{
        {"_id", 0},
        {"a", 1},
        {"b", 2},
        {"results", std::vector<Value>{Value{Document{{"_id", 1}, {"to", false}}}}}};
    ASSERT_DOCUMENT_EQ(actualResult, expectedResult);
}

}  // namespace
}  // namespace mongo
