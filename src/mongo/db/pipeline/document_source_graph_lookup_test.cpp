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

#include "mongo/db/pipeline/document_source.h"

#include <algorithm>
#include <deque>

#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_value_test_util.h"
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
class MockMongodImplementation final : public DocumentSourceNeedsMongod::MongodInterface {
public:
    MockMongodImplementation(std::deque<Document> documents) : _documents(documents) {}

    void setOperationContext(OperationContext* opCtx) final {
        MONGO_UNREACHABLE;
    }

    DBClientBase* directClient() final {
        MONGO_UNREACHABLE;
    }

    bool isSharded(const NamespaceString& ns) final {
        MONGO_UNREACHABLE;
    }

    BSONObj insert(const NamespaceString& ns, const std::vector<BSONObj>& objs) final {
        MONGO_UNREACHABLE;
    }

    CollectionIndexUsageMap getIndexStats(OperationContext* opCtx,
                                          const NamespaceString& ns) final {
        MONGO_UNREACHABLE;
    }

    void appendLatencyStats(const NamespaceString& nss, BSONObjBuilder* builder) const final {
        MONGO_UNREACHABLE;
    }

    BSONObj getCollectionOptions(const NamespaceString& nss) final {
        MONGO_UNREACHABLE;
    }

    Status renameIfOptionsAndIndexesHaveNotChanged(
        const BSONObj& renameCommandObj,
        const NamespaceString& targetNs,
        const BSONObj& originalCollectionOptions,
        const std::list<BSONObj>& originalIndexes) final {
        MONGO_UNREACHABLE;
    }

    StatusWith<boost::intrusive_ptr<Pipeline>> makePipeline(
        const std::vector<BSONObj>& rawPipeline,
        const boost::intrusive_ptr<ExpressionContext>& expCtx) final {
        auto pipeline = Pipeline::parse(rawPipeline, expCtx);
        if (!pipeline.isOK()) {
            return pipeline.getStatus();
        }

        pipeline.getValue()->addInitialSource(DocumentSourceMock::create(_documents));
        pipeline.getValue()->injectExpressionContext(expCtx);
        pipeline.getValue()->optimizePipeline();

        return pipeline;
    }

private:
    std::deque<Document> _documents;
};

TEST_F(DocumentSourceGraphLookUpTest,
       ShouldErrorWhenDoingInitialMatchIfDocumentInFromCollectionIsMissingId) {
    auto expCtx = getExpCtx();

    std::deque<Document> inputs{Document{{"_id", 0}}};
    auto inputMock = DocumentSourceMock::create(std::move(inputs));

    std::deque<Document> fromContents{Document{{"to", 0}}};

    NamespaceString fromNs("test", "graph_lookup");
    expCtx->resolvedNamespaces[fromNs.coll()] = {fromNs, std::vector<BSONObj>{}};
    auto graphLookupStage = DocumentSourceGraphLookUp::create(expCtx,
                                                              fromNs,
                                                              "results",
                                                              "from",
                                                              "to",
                                                              ExpressionFieldPath::create("_id"),
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

    std::deque<Document> inputs{Document{{"_id", 0}}};
    auto inputMock = DocumentSourceMock::create(std::move(inputs));

    std::deque<Document> fromContents{Document{{"_id", "a"}, {"to", 0}, {"from", 1}},
                                      Document{{"to", 1}}};

    NamespaceString fromNs("test", "graph_lookup");
    expCtx->resolvedNamespaces[fromNs.coll()] = {fromNs, std::vector<BSONObj>{}};
    auto graphLookupStage = DocumentSourceGraphLookUp::create(expCtx,
                                                              fromNs,
                                                              "results",
                                                              "from",
                                                              "to",
                                                              ExpressionFieldPath::create("_id"),
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

    std::deque<Document> inputs{Document{{"_id", 0}}};
    auto inputMock = DocumentSourceMock::create(std::move(inputs));

    std::deque<Document> fromContents{Document{{"to", 0}}};

    NamespaceString fromNs("test", "graph_lookup");
    expCtx->resolvedNamespaces[fromNs.coll()] = {fromNs, std::vector<BSONObj>{}};
    auto graphLookupStage = DocumentSourceGraphLookUp::create(expCtx,
                                                              fromNs,
                                                              "results",
                                                              "from",
                                                              "to",
                                                              ExpressionFieldPath::create("_id"),
                                                              boost::none,
                                                              boost::none,
                                                              boost::none);
    graphLookupStage->injectMongodInterface(
        std::make_shared<MockMongodImplementation>(std::move(fromContents)));

    auto unwindStage = DocumentSourceUnwind::create(expCtx, "results", false, boost::none);
    auto pipeline =
        unittest::assertGet(Pipeline::create({inputMock, graphLookupStage, unwindStage}, expCtx));
    pipeline->optimizePipeline();

    ASSERT_THROWS_CODE(pipeline->output()->getNext(), UserException, 40271);
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

    std::deque<Document> inputs{Document{{"_id", 0}}};
    auto inputMock = DocumentSourceMock::create(std::move(inputs));

    Document to0from1{{"_id", "a"}, {"to", 0}, {"from", 1}};
    Document to0from2{{"_id", "a"}, {"to", 0}, {"from", 2}};
    Document to1{{"_id", "b"}, {"to", 1}};
    Document to2{{"_id", "c"}, {"to", 2}};
    std::deque<Document> fromContents{to1, to2, to0from1, to0from2};

    NamespaceString fromNs("test", "graph_lookup");
    expCtx->resolvedNamespaces[fromNs.coll()] = {fromNs, std::vector<BSONObj>{}};
    auto graphLookupStage = DocumentSourceGraphLookUp::create(expCtx,
                                                              fromNs,
                                                              "results",
                                                              "from",
                                                              "to",
                                                              ExpressionFieldPath::create("_id"),
                                                              boost::none,
                                                              boost::none,
                                                              boost::none);
    graphLookupStage->setSource(inputMock.get());
    graphLookupStage->injectMongodInterface(
        std::make_shared<MockMongodImplementation>(std::move(fromContents)));
    auto pipeline = unittest::assertGet(Pipeline::create({inputMock, graphLookupStage}, expCtx));

    auto next = pipeline->output()->getNext();
    ASSERT(next);

    ASSERT_EQ(2U, next->size());
    ASSERT_VALUE_EQ(Value(0), next->getField("_id"));

    auto resultsValue = next->getField("results");
    ASSERT(resultsValue.isArray());
    auto resultsArray = resultsValue.getArray();

    // Since 'to0from1' and 'to0from2' have the same _id, we should end up only exploring the path
    // through one of them.
    if (arrayContains(expCtx, resultsArray, Value(to0from1))) {
        // If 'to0from1' was returned, then we should see 'to1' and nothing else.
        ASSERT(arrayContains(expCtx, resultsArray, Value(to1)));
        ASSERT_EQ(2U, resultsArray.size());

        next = pipeline->output()->getNext();
        ASSERT(!next);
    } else if (arrayContains(expCtx, resultsArray, Value(to0from2))) {
        // If 'to0from2' was returned, then we should see 'to2' and nothing else.
        ASSERT(arrayContains(expCtx, resultsArray, Value(to2)));
        ASSERT_EQ(2U, resultsArray.size());

        next = pipeline->output()->getNext();
        ASSERT(!next);
    } else {
        FAIL(str::stream() << "Expected either [ " << to0from1.toString() << " ] or [ "
                           << to0from2.toString()
                           << " ] but found [ "
                           << next->toString()
                           << " ]");
    }
}

}  // namespace
}  // namespace mongo
