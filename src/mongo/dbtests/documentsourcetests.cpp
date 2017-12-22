// documentsourcetests.cpp : Unit tests for DocumentSource classes.

/**
 *    Copyright (C) 2012-2014 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/multi_plan.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/mock_yield_policies.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/stage_builder.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

using boost::intrusive_ptr;
using std::unique_ptr;
using std::vector;

static const NamespaceString nss("unittests.documentsourcetests");
static const BSONObj metaTextScore = BSON("$meta"
                                          << "textScore");

BSONObj toBson(const intrusive_ptr<DocumentSource>& source) {
    vector<Value> arr;
    source->serializeToArray(arr);
    ASSERT_EQUALS(arr.size(), 1UL);
    return arr[0].getDocument().toBson();
}

class DocumentSourceCursorTest : public unittest::Test {
public:
    DocumentSourceCursorTest()
        : client(_opCtx.get()),
          _ctx(new ExpressionContextForTest(_opCtx.get(), AggregationRequest(nss, {}))) {
        _ctx->tempDir = storageGlobalParams.dbpath + "/_tmp";
    }

    virtual ~DocumentSourceCursorTest() {
        client.dropCollection(nss.ns());
    }

protected:
    void createSource(boost::optional<BSONObj> hint = boost::none) {
        // clean up first if this was called before
        _source.reset();

        OldClientWriteContext ctx(opCtx(), nss.ns());

        auto qr = stdx::make_unique<QueryRequest>(nss);
        if (hint) {
            qr->setHint(*hint);
        }
        auto cq = uassertStatusOK(CanonicalQuery::canonicalize(opCtx(), std::move(qr)));

        auto exec = uassertStatusOK(
            getExecutor(opCtx(), ctx.getCollection(), std::move(cq), PlanExecutor::NO_YIELD, 0));

        exec->saveState();
        _source = DocumentSourceCursor::create(ctx.getCollection(), std::move(exec), _ctx);
    }

    intrusive_ptr<ExpressionContextForTest> ctx() {
        return _ctx;
    }

    DocumentSourceCursor* source() {
        return _source.get();
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

protected:
    const ServiceContext::UniqueOperationContext _opCtx = cc().makeOperationContext();
    DBDirectClient client;

private:
    // It is important that these are ordered to ensure correct destruction order.
    intrusive_ptr<ExpressionContextForTest> _ctx;
    intrusive_ptr<DocumentSourceCursor> _source;
};

/** Create a DocumentSourceCursor. */
TEST_F(DocumentSourceCursorTest, Empty) {
    createSource();
    // The DocumentSourceCursor doesn't hold a read lock.
    ASSERT(!opCtx()->lockState()->isReadLocked());
    // The collection is empty, so the source produces no results.
    ASSERT(source()->getNext().isEOF());
    // Exhausting the source releases the read lock.
    ASSERT(!opCtx()->lockState()->isReadLocked());
}

/** Iterate a DocumentSourceCursor. */
TEST_F(DocumentSourceCursorTest, Iterate) {
    client.insert(nss.ns(), BSON("a" << 1));
    createSource();
    // The DocumentSourceCursor doesn't hold a read lock.
    ASSERT(!opCtx()->lockState()->isReadLocked());
    // The cursor will produce the expected result.
    auto next = source()->getNext();
    ASSERT(next.isAdvanced());
    ASSERT_VALUE_EQ(Value(1), next.getDocument().getField("a"));
    // There are no more results.
    ASSERT(source()->getNext().isEOF());
    // Exhausting the source releases the read lock.
    ASSERT(!opCtx()->lockState()->isReadLocked());
}

/** Dispose of a DocumentSourceCursor. */
TEST_F(DocumentSourceCursorTest, Dispose) {
    createSource();
    // The DocumentSourceCursor doesn't hold a read lock.
    ASSERT(!opCtx()->lockState()->isReadLocked());
    source()->dispose();
    // Releasing the cursor releases the read lock.
    ASSERT(!opCtx()->lockState()->isReadLocked());
    // The source is marked as exhausted.
    ASSERT(source()->getNext().isEOF());
}

/** Iterate a DocumentSourceCursor and then dispose of it. */
TEST_F(DocumentSourceCursorTest, IterateDispose) {
    client.insert(nss.ns(), BSON("a" << 1));
    client.insert(nss.ns(), BSON("a" << 2));
    client.insert(nss.ns(), BSON("a" << 3));
    createSource();
    // The result is as expected.
    auto next = source()->getNext();
    ASSERT(next.isAdvanced());
    ASSERT_VALUE_EQ(Value(1), next.getDocument().getField("a"));
    // The next result is as expected.
    next = source()->getNext();
    ASSERT(next.isAdvanced());
    ASSERT_VALUE_EQ(Value(2), next.getDocument().getField("a"));
    // The DocumentSourceCursor doesn't hold a read lock.
    ASSERT(!opCtx()->lockState()->isReadLocked());
    source()->dispose();
    // Disposing of the source releases the lock.
    ASSERT(!opCtx()->lockState()->isReadLocked());
    // The source cannot be advanced further.
    ASSERT(source()->getNext().isEOF());
}

/** Set a value or await an expected value. */
class PendingValue {
public:
    PendingValue(int initialValue) : _value(initialValue) {}
    void set(int newValue) {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _value = newValue;
        _condition.notify_all();
    }
    void await(int expectedValue) const {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        while (_value != expectedValue) {
            _condition.wait(lk);
        }
    }

private:
    int _value;
    mutable stdx::mutex _mutex;
    mutable stdx::condition_variable _condition;
};


/** Test coalescing a limit into a cursor */
TEST_F(DocumentSourceCursorTest, LimitCoalesce) {
    client.insert(nss.ns(), BSON("a" << 1));
    client.insert(nss.ns(), BSON("a" << 2));
    client.insert(nss.ns(), BSON("a" << 3));
    createSource();

    Pipeline::SourceContainer container;
    container.push_back(source());
    container.push_back(DocumentSourceLimit::create(ctx(), 10));
    source()->optimizeAt(container.begin(), &container);

    // initial limit becomes limit of cursor
    ASSERT_EQUALS(container.size(), 1U);
    ASSERT_EQUALS(source()->getLimit(), 10);

    container.push_back(DocumentSourceLimit::create(ctx(), 2));
    source()->optimizeAt(container.begin(), &container);
    // smaller limit lowers cursor limit
    ASSERT_EQUALS(container.size(), 1U);
    ASSERT_EQUALS(source()->getLimit(), 2);

    container.push_back(DocumentSourceLimit::create(ctx(), 3));
    source()->optimizeAt(container.begin(), &container);
    // higher limit doesn't effect cursor limit
    ASSERT_EQUALS(container.size(), 1U);
    ASSERT_EQUALS(source()->getLimit(), 2);

    // The cursor allows exactly 2 documents through
    ASSERT(source()->getNext().isAdvanced());
    ASSERT(source()->getNext().isAdvanced());
    ASSERT(source()->getNext().isEOF());
}

//
// Test cursor output sort.
//
TEST_F(DocumentSourceCursorTest, CollectionScanProvidesNoSort) {
    createSource(BSON("$natural" << 1));
    ASSERT_EQ(source()->getOutputSorts().size(), 0U);
    source()->dispose();
}

TEST_F(DocumentSourceCursorTest, IndexScanProvidesSortOnKeys) {
    client.createIndex(nss.ns(), BSON("a" << 1));
    createSource(BSON("a" << 1));

    ASSERT_EQ(source()->getOutputSorts().size(), 1U);
    ASSERT_EQ(source()->getOutputSorts().count(BSON("a" << 1)), 1U);
    source()->dispose();
}

TEST_F(DocumentSourceCursorTest, ReverseIndexScanProvidesSort) {
    client.createIndex(nss.ns(), BSON("a" << -1));
    createSource(BSON("a" << -1));

    ASSERT_EQ(source()->getOutputSorts().size(), 1U);
    ASSERT_EQ(source()->getOutputSorts().count(BSON("a" << -1)), 1U);
    source()->dispose();
}

TEST_F(DocumentSourceCursorTest, CompoundIndexScanProvidesMultipleSorts) {
    client.createIndex(nss.ns(), BSON("a" << 1 << "b" << -1));
    createSource(BSON("a" << 1 << "b" << -1));

    ASSERT_EQ(source()->getOutputSorts().size(), 2U);
    ASSERT_EQ(source()->getOutputSorts().count(BSON("a" << 1)), 1U);
    ASSERT_EQ(source()->getOutputSorts().count(BSON("a" << 1 << "b" << -1)), 1U);
    source()->dispose();
}

TEST_F(DocumentSourceCursorTest, SerializationRespectsExplainModes) {
    createSource();

    {
        // Nothing serialized when no explain mode specified.
        auto explainResult = source()->serialize();
        ASSERT_TRUE(explainResult.missing());
    }

    {
        auto explainResult = source()->serialize(ExplainOptions::Verbosity::kQueryPlanner);
        ASSERT_FALSE(explainResult["$cursor"]["queryPlanner"].missing());
        ASSERT_TRUE(explainResult["$cursor"]["executionStats"].missing());
    }

    {
        auto explainResult = source()->serialize(ExplainOptions::Verbosity::kExecStats);
        ASSERT_FALSE(explainResult["$cursor"]["queryPlanner"].missing());
        ASSERT_FALSE(explainResult["$cursor"]["executionStats"].missing());
        ASSERT_TRUE(explainResult["$cursor"]["executionStats"]["allPlansExecution"].missing());
    }

    {
        auto explainResult =
            source()->serialize(ExplainOptions::Verbosity::kExecAllPlans).getDocument();
        ASSERT_FALSE(explainResult["$cursor"]["queryPlanner"].missing());
        ASSERT_FALSE(explainResult["$cursor"]["executionStats"].missing());
        ASSERT_FALSE(explainResult["$cursor"]["executionStats"]["allPlansExecution"].missing());
    }
    source()->dispose();
}

TEST_F(DocumentSourceCursorTest, TailableAwaitDataCursorShouldErrorAfterTimeout) {
    // Make sure the collection exists, otherwise we'll default to a NO_YIELD yield policy.
    const bool capped = true;
    const long long cappedSize = 1024;
    ASSERT_TRUE(client.createCollection(nss.ns(), cappedSize, capped));
    client.insert(nss.ns(), BSON("a" << 1));

    // Make a tailable collection scan wrapped up in a PlanExecutor.
    AutoGetCollectionForRead readLock(opCtx(), nss);
    auto workingSet = stdx::make_unique<WorkingSet>();
    CollectionScanParams collScanParams;
    collScanParams.collection = readLock.getCollection();
    collScanParams.tailable = true;
    auto filter = BSON("a" << 1);
    auto matchExpression = uassertStatusOK(MatchExpressionParser::parse(filter, ctx()));
    auto collectionScan = stdx::make_unique<CollectionScan>(
        opCtx(), collScanParams, workingSet.get(), matchExpression.get());
    auto queryRequest = stdx::make_unique<QueryRequest>(nss);
    queryRequest->setFilter(filter);
    queryRequest->setTailableMode(TailableMode::kTailableAndAwaitData);
    auto canonicalQuery = unittest::assertGet(
        CanonicalQuery::canonicalize(opCtx(), std::move(queryRequest), nullptr));
    auto planExecutor =
        uassertStatusOK(PlanExecutor::make(opCtx(),
                                           std::move(workingSet),
                                           std::move(collectionScan),
                                           std::move(canonicalQuery),
                                           readLock.getCollection(),
                                           PlanExecutor::YieldPolicy::ALWAYS_TIME_OUT));

    // Make a DocumentSourceCursor.
    ctx()->tailableMode = TailableMode::kTailableAndAwaitData;
    // DocumentSourceCursor expects a PlanExecutor that has had its state saved.
    planExecutor->saveState();
    auto cursor =
        DocumentSourceCursor::create(readLock.getCollection(), std::move(planExecutor), ctx());

    ON_BLOCK_EXIT([cursor]() { cursor->dispose(); });
    ASSERT_THROWS_CODE(cursor->getNext().isEOF(), AssertionException, ErrorCodes::QueryPlanKilled);
}

TEST_F(DocumentSourceCursorTest, NonAwaitDataCursorShouldErrorAfterTimeout) {
    // Make sure the collection exists, otherwise we'll default to a NO_YIELD yield policy.
    ASSERT_TRUE(client.createCollection(nss.ns()));
    client.insert(nss.ns(), BSON("a" << 1));

    // Make a tailable collection scan wrapped up in a PlanExecutor.
    AutoGetCollectionForRead readLock(opCtx(), nss);
    auto workingSet = stdx::make_unique<WorkingSet>();
    CollectionScanParams collScanParams;
    collScanParams.collection = readLock.getCollection();
    auto filter = BSON("a" << 1);
    auto matchExpression = uassertStatusOK(MatchExpressionParser::parse(filter, ctx()));
    auto collectionScan = stdx::make_unique<CollectionScan>(
        opCtx(), collScanParams, workingSet.get(), matchExpression.get());
    auto queryRequest = stdx::make_unique<QueryRequest>(nss);
    queryRequest->setFilter(filter);
    auto canonicalQuery = unittest::assertGet(
        CanonicalQuery::canonicalize(opCtx(), std::move(queryRequest), nullptr));
    auto planExecutor =
        uassertStatusOK(PlanExecutor::make(opCtx(),
                                           std::move(workingSet),
                                           std::move(collectionScan),
                                           std::move(canonicalQuery),
                                           readLock.getCollection(),
                                           PlanExecutor::YieldPolicy::ALWAYS_TIME_OUT));

    // Make a DocumentSourceCursor.
    ctx()->tailableMode = TailableMode::kNormal;
    // DocumentSourceCursor expects a PlanExecutor that has had its state saved.
    planExecutor->saveState();
    auto cursor =
        DocumentSourceCursor::create(readLock.getCollection(), std::move(planExecutor), ctx());

    ON_BLOCK_EXIT([cursor]() { cursor->dispose(); });
    ASSERT_THROWS_CODE(cursor->getNext().isEOF(), AssertionException, ErrorCodes::QueryPlanKilled);
}

TEST_F(DocumentSourceCursorTest, TailableAwaitDataCursorShouldErrorAfterBeingKilled) {
    // Make sure the collection exists, otherwise we'll default to a NO_YIELD yield policy.
    const bool capped = true;
    const long long cappedSize = 1024;
    ASSERT_TRUE(client.createCollection(nss.ns(), cappedSize, capped));
    client.insert(nss.ns(), BSON("a" << 1));

    // Make a tailable collection scan wrapped up in a PlanExecutor.
    AutoGetCollectionForRead readLock(opCtx(), nss);
    auto workingSet = stdx::make_unique<WorkingSet>();
    CollectionScanParams collScanParams;
    collScanParams.collection = readLock.getCollection();
    collScanParams.tailable = true;
    auto filter = BSON("a" << 1);
    auto matchExpression = uassertStatusOK(MatchExpressionParser::parse(filter, ctx()));
    auto collectionScan = stdx::make_unique<CollectionScan>(
        opCtx(), collScanParams, workingSet.get(), matchExpression.get());
    auto queryRequest = stdx::make_unique<QueryRequest>(nss);
    queryRequest->setFilter(filter);
    queryRequest->setTailableMode(TailableMode::kTailableAndAwaitData);
    auto canonicalQuery = unittest::assertGet(
        CanonicalQuery::canonicalize(opCtx(), std::move(queryRequest), nullptr));
    auto planExecutor =
        uassertStatusOK(PlanExecutor::make(opCtx(),
                                           std::move(workingSet),
                                           std::move(collectionScan),
                                           std::move(canonicalQuery),
                                           readLock.getCollection(),
                                           PlanExecutor::YieldPolicy::ALWAYS_MARK_KILLED));

    // Make a DocumentSourceCursor.
    ctx()->tailableMode = TailableMode::kTailableAndAwaitData;
    // DocumentSourceCursor expects a PlanExecutor that has had its state saved.
    planExecutor->saveState();
    auto cursor =
        DocumentSourceCursor::create(readLock.getCollection(), std::move(planExecutor), ctx());

    ON_BLOCK_EXIT([cursor]() { cursor->dispose(); });
    ASSERT_THROWS_CODE(cursor->getNext().isEOF(), AssertionException, ErrorCodes::QueryPlanKilled);
}

TEST_F(DocumentSourceCursorTest, NormalCursorShouldErrorAfterBeingKilled) {
    // Make sure the collection exists, otherwise we'll default to a NO_YIELD yield policy.
    ASSERT_TRUE(client.createCollection(nss.ns()));
    client.insert(nss.ns(), BSON("a" << 1));

    // Make a tailable collection scan wrapped up in a PlanExecutor.
    AutoGetCollectionForRead readLock(opCtx(), nss);
    auto workingSet = stdx::make_unique<WorkingSet>();
    CollectionScanParams collScanParams;
    collScanParams.collection = readLock.getCollection();
    auto filter = BSON("a" << 1);
    auto matchExpression = uassertStatusOK(MatchExpressionParser::parse(filter, ctx()));
    auto collectionScan = stdx::make_unique<CollectionScan>(
        opCtx(), collScanParams, workingSet.get(), matchExpression.get());
    auto queryRequest = stdx::make_unique<QueryRequest>(nss);
    queryRequest->setFilter(filter);
    auto canonicalQuery = unittest::assertGet(
        CanonicalQuery::canonicalize(opCtx(), std::move(queryRequest), nullptr));
    auto planExecutor =
        uassertStatusOK(PlanExecutor::make(opCtx(),
                                           std::move(workingSet),
                                           std::move(collectionScan),
                                           std::move(canonicalQuery),
                                           readLock.getCollection(),
                                           PlanExecutor::YieldPolicy::ALWAYS_MARK_KILLED));

    // Make a DocumentSourceCursor.
    ctx()->tailableMode = TailableMode::kNormal;
    // DocumentSourceCursor expects a PlanExecutor that has had its state saved.
    planExecutor->saveState();
    auto cursor =
        DocumentSourceCursor::create(readLock.getCollection(), std::move(planExecutor), ctx());

    ON_BLOCK_EXIT([cursor]() { cursor->dispose(); });
    ASSERT_THROWS_CODE(cursor->getNext().isEOF(), AssertionException, ErrorCodes::QueryPlanKilled);
}

}  // namespace
}  // namespace mongo
