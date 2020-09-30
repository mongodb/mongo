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

#include <memory>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/fetch.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/plan_executor_pipeline.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/dbtests/dbtests.h"

namespace mongo {
namespace {

using std::shared_ptr;
using std::string;
using std::unique_ptr;

static const NamespaceString nss("unittests.QueryPlanExecutor");

class PlanExecutorTest : public unittest::Test {
public:
    PlanExecutorTest() : _client(&_opCtx) {}

    virtual ~PlanExecutorTest() {
        _client.dropCollection(nss.ns());
    }

    void addIndex(const BSONObj& obj) {
        ASSERT_OK(dbtests::createIndex(&_opCtx, nss.ns(), obj));
    }

    void insert(const BSONObj& obj) {
        _client.insert(nss.ns(), obj);
    }

    void remove(const BSONObj& obj) {
        _client.remove(nss.ns(), obj);
    }

    void dropCollection() {
        _client.dropCollection(nss.ns());
    }

    void update(BSONObj& query, BSONObj& updateSpec) {
        _client.update(nss.ns(), query, updateSpec, false, false);
    }

    /**
     * Given a match expression, represented as the BSON object 'filterObj', create a PlanExecutor
     * capable of executing a simple collection scan.
     */
    unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeCollScanExec(
        const CollectionPtr& coll,
        BSONObj& filterObj,
        PlanYieldPolicy::YieldPolicy yieldPolicy = PlanYieldPolicy::YieldPolicy::YIELD_MANUAL,
        TailableModeEnum tailableMode = TailableModeEnum::kNormal) {
        CollectionScanParams csparams;
        csparams.direction = CollectionScanParams::FORWARD;
        unique_ptr<WorkingSet> ws(new WorkingSet());

        // Canonicalize the query.
        auto qr = std::make_unique<QueryRequest>(nss);
        qr->setFilter(filterObj);
        qr->setTailableMode(tailableMode);
        auto statusWithCQ = CanonicalQuery::canonicalize(&_opCtx, std::move(qr));
        ASSERT_OK(statusWithCQ.getStatus());
        unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());
        verify(nullptr != cq.get());

        // Make the stage.
        unique_ptr<PlanStage> root(
            new CollectionScan(cq->getExpCtxRaw(), coll, csparams, ws.get(), cq.get()->root()));

        // Hand the plan off to the executor.
        auto statusWithPlanExecutor = plan_executor_factory::make(
            std::move(cq), std::move(ws), std::move(root), &coll, yieldPolicy);
        ASSERT_OK(statusWithPlanExecutor.getStatus());
        return std::move(statusWithPlanExecutor.getValue());
    }

    /**
     * @param indexSpec -- a BSONObj giving the index over which to
     *   scan, e.g. {_id: 1}.
     * @param start -- the lower bound (inclusive) at which to start
     *   the index scan
     * @param end -- the lower bound (inclusive) at which to end the
     *   index scan
     *
     * Returns a PlanExecutor capable of executing an index scan
     * over the specified index with the specified bounds.
     */
    unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeIndexScanExec(
        Database* db, const CollectionPtr& coll, BSONObj& indexSpec, int start, int end) {
        // Build the index scan stage.
        auto descriptor = getIndex(db, indexSpec);
        IndexScanParams ixparams(&_opCtx, descriptor);
        ixparams.bounds.isSimpleRange = true;
        ixparams.bounds.startKey = BSON("" << start);
        ixparams.bounds.endKey = BSON("" << end);
        ixparams.bounds.boundInclusion = BoundInclusion::kIncludeBothStartAndEndKeys;

        unique_ptr<WorkingSet> ws(new WorkingSet());
        auto ixscan = std::make_unique<IndexScan>(_expCtx.get(), coll, ixparams, ws.get(), nullptr);
        unique_ptr<PlanStage> root =
            std::make_unique<FetchStage>(_expCtx.get(), ws.get(), std::move(ixscan), nullptr, coll);

        auto qr = std::make_unique<QueryRequest>(nss);
        auto statusWithCQ = CanonicalQuery::canonicalize(&_opCtx, std::move(qr));
        verify(statusWithCQ.isOK());
        unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());
        verify(nullptr != cq.get());

        // Hand the plan off to the executor.
        auto statusWithPlanExecutor =
            plan_executor_factory::make(std::move(cq),
                                        std::move(ws),
                                        std::move(root),
                                        &coll,
                                        PlanYieldPolicy::YieldPolicy::YIELD_MANUAL);
        ASSERT_OK(statusWithPlanExecutor.getStatus());
        return std::move(statusWithPlanExecutor.getValue());
    }

protected:
    const ServiceContext::UniqueOperationContext _opCtxPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_opCtxPtr;

    boost::intrusive_ptr<ExpressionContext> _expCtx =
        make_intrusive<ExpressionContext>(&_opCtx, nullptr, nss);

private:
    const IndexDescriptor* getIndex(Database* db, const BSONObj& obj) {
        CollectionPtr collection =
            CollectionCatalog::get(&_opCtx).lookupCollectionByNamespace(&_opCtx, nss);
        std::vector<const IndexDescriptor*> indexes;
        collection->getIndexCatalog()->findIndexesByKeyPattern(&_opCtx, obj, false, &indexes);
        ASSERT_LTE(indexes.size(), 1U);
        return indexes.size() == 0 ? nullptr : indexes[0];
    }

    DBDirectClient _client;
};

/**
 * Test dropping the collection while an agg PlanExecutor is doing an index scan.
 */
TEST_F(PlanExecutorTest, DropIndexScanAgg) {
    dbtests::WriteContextForTests ctx(&_opCtx, nss.ns());

    insert(BSON("_id" << 1 << "a" << 6));
    insert(BSON("_id" << 2 << "a" << 7));
    insert(BSON("_id" << 3 << "a" << 8));
    BSONObj indexSpec = BSON("a" << 1);
    addIndex(indexSpec);

    CollectionPtr collection = ctx.getCollection();

    // Create the aggregation pipeline.
    std::vector<BSONObj> rawPipeline = {fromjson("{$match: {a: {$gte: 7, $lte: 10}}}")};

    // Create an "inner" plan executor and register it with the cursor manager so that it can
    // get notified when the collection is dropped.
    unique_ptr<PlanExecutor, PlanExecutor::Deleter> innerExec(
        makeIndexScanExec(ctx.db(), collection, indexSpec, 7, 10));

    // Wrap the "inner" plan executor in a DocumentSourceCursor and add it as the first source
    // in the pipeline.
    innerExec->saveState();
    auto cursorSource = DocumentSourceCursor::create(
        collection, std::move(innerExec), _expCtx, DocumentSourceCursor::CursorType::kRegular);
    auto pipeline = Pipeline::create({cursorSource}, _expCtx);

    auto outerExec =
        plan_executor_factory::make(_expCtx, std::move(pipeline), false /* isChangeStream */);

    dropCollection();

    // Verify that the aggregation pipeline returns an error because its "inner" plan executor
    // has been killed due to the collection being dropped.
    BSONObj objOut;
    ASSERT_THROWS_CODE(
        outerExec->getNext(&objOut, nullptr), AssertionException, ErrorCodes::QueryPlanKilled);
}

TEST_F(PlanExecutorTest, ShouldReportErrorIfExceedsTimeLimitDuringYield) {
    dbtests::WriteContextForTests ctx(&_opCtx, nss.ns());
    insert(BSON("_id" << 1));
    insert(BSON("_id" << 2));

    BSONObj filterObj = fromjson("{_id: {$gt: 0}}");

    const CollectionPtr& coll = ctx.getCollection();
    auto exec = makeCollScanExec(coll, filterObj, PlanYieldPolicy::YieldPolicy::ALWAYS_TIME_OUT);

    BSONObj resultObj;
    ASSERT_THROWS_CODE_AND_WHAT(exec->getNext(&resultObj, nullptr),
                                DBException,
                                ErrorCodes::ExceededTimeLimit,
                                "Using AlwaysTimeOutYieldPolicy");
}

TEST_F(PlanExecutorTest, ShouldReportErrorIfKilledDuringYieldButIsTailableAndAwaitData) {
    dbtests::WriteContextForTests ctx(&_opCtx, nss.ns());
    insert(BSON("_id" << 1));
    insert(BSON("_id" << 2));

    BSONObj filterObj = fromjson("{_id: {$gt: 0}}");

    const CollectionPtr& coll = ctx.getCollection();
    auto exec = makeCollScanExec(coll,
                                 filterObj,
                                 PlanYieldPolicy::YieldPolicy::ALWAYS_TIME_OUT,
                                 TailableModeEnum::kTailableAndAwaitData);

    BSONObj resultObj;
    ASSERT_THROWS_CODE_AND_WHAT(exec->getNext(&resultObj, nullptr),
                                DBException,
                                ErrorCodes::ExceededTimeLimit,
                                "Using AlwaysTimeOutYieldPolicy");
}

TEST_F(PlanExecutorTest, ShouldNotSwallowExceedsTimeLimitDuringYieldButIsTailableButNotAwaitData) {
    dbtests::WriteContextForTests ctx(&_opCtx, nss.ns());
    insert(BSON("_id" << 1));
    insert(BSON("_id" << 2));

    BSONObj filterObj = fromjson("{_id: {$gt: 0}}");

    const CollectionPtr& coll = ctx.getCollection();
    auto exec = makeCollScanExec(coll,
                                 filterObj,
                                 PlanYieldPolicy::YieldPolicy::ALWAYS_TIME_OUT,
                                 TailableModeEnum::kTailable);

    BSONObj resultObj;
    ASSERT_THROWS_CODE_AND_WHAT(exec->getNext(&resultObj, nullptr),
                                DBException,
                                ErrorCodes::ExceededTimeLimit,
                                "Using AlwaysTimeOutYieldPolicy");
}

TEST_F(PlanExecutorTest, ShouldReportErrorIfKilledDuringYield) {
    dbtests::WriteContextForTests ctx(&_opCtx, nss.ns());
    insert(BSON("_id" << 1));
    insert(BSON("_id" << 2));

    BSONObj filterObj = fromjson("{_id: {$gt: 0}}");

    const CollectionPtr& coll = ctx.getCollection();
    auto exec = makeCollScanExec(coll, filterObj, PlanYieldPolicy::YieldPolicy::ALWAYS_MARK_KILLED);

    BSONObj resultObj;
    ASSERT_THROWS_CODE_AND_WHAT(exec->getNext(&resultObj, nullptr),
                                DBException,
                                ErrorCodes::QueryPlanKilled,
                                "Using AlwaysPlanKilledYieldPolicy");
}

class PlanExecutorSnapshotTest : public PlanExecutorTest {
protected:
    CollectionPtr setupCollection() {
        insert(BSON("_id" << 1 << "a" << 1));
        insert(BSON("_id" << 2 << "a" << 2 << "payload"
                          << "x"));
        insert(BSON("_id" << 3 << "a" << 3));
        insert(BSON("_id" << 4 << "a" << 4));

        return CollectionCatalog::get(&_opCtx).lookupCollectionByNamespace(&_opCtx, nss);
    }

    /**
     * Increases a document's size dramatically such that the document
     * exceeds the available padding and must be moved to the end of
     * the collection.
     */
    void forceDocumentMove() {
        BSONObj query = BSON("_id" << 2);
        BSONObj updateSpec = BSON("$set" << BSON("payload" << payload8k()));
        update(query, updateSpec);
    }

    std::string payload8k() {
        return std::string(8 * 1024, 'x');
    }

    /**
     * Given an array of ints, 'expectedIds', and a PlanExecutor,
     * 'exec', uses the executor to iterate through the collection. While
     * iterating, asserts that the _id of each successive document equals
     * the respective integer in 'expectedIds'.
     */
    void checkIds(int* expectedIds, PlanExecutor* exec) {
        BSONObj objOut;
        int idcount = 0;
        PlanExecutor::ExecState state;
        while (PlanExecutor::ADVANCED == (state = exec->getNext(&objOut, nullptr))) {
            ASSERT_EQUALS(expectedIds[idcount], objOut["_id"].numberInt());
            ++idcount;
        }

        ASSERT_EQUALS(PlanExecutor::IS_EOF, state);
    }
};

/**
 * Create a scenario in which the same document is returned
 * twice due to a concurrent document move and collection
 * scan.
 */
TEST_F(PlanExecutorSnapshotTest, SnapshotControl) {
    dbtests::WriteContextForTests ctx(&_opCtx, nss.ns());
    setupCollection();

    BSONObj filterObj = fromjson("{a: {$gte: 2}}");

    const CollectionPtr& coll = ctx.getCollection();
    auto exec = makeCollScanExec(coll, filterObj);

    BSONObj objOut;
    ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&objOut, nullptr));
    ASSERT_EQUALS(2, objOut["a"].numberInt());

    forceDocumentMove();

    int ids[] = {3, 4, 2};
    checkIds(ids, exec.get());
}

/**
 * A snapshot is really just a hint that means scan the _id index.
 * Make sure that we do not see the document move with an _id
 * index scan.
 */
TEST_F(PlanExecutorSnapshotTest, SnapshotTest) {
    dbtests::WriteContextForTests ctx(&_opCtx, nss.ns());
    auto coll = setupCollection();
    BSONObj indexSpec = BSON("_id" << 1);
    addIndex(indexSpec);

    BSONObj filterObj = fromjson("{a: {$gte: 2}}");
    auto exec = makeIndexScanExec(ctx.db(), coll, indexSpec, 2, 5);

    BSONObj objOut;
    ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&objOut, nullptr));
    ASSERT_EQUALS(2, objOut["a"].numberInt());

    forceDocumentMove();

    // Since this time we're scanning the _id index,
    // we should not see the moved document again.
    int ids[] = {3, 4};
    checkIds(ids, exec.get());
}

}  // namespace
}  // namespace mongo
