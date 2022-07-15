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

/**
 * This file tests db/exec/collection_scan.cpp.
 */


#include "mongo/platform/basic.h"

#include <fmt/printf.h>
#include <memory>

#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/catalog/clustered_collection_options_gen.h"
#include "mongo/db/catalog/clustered_collection_util.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace query_stage_collection_scan {

using std::unique_ptr;
using std::vector;

static const NamespaceString nss{"unittests.QueryStageCollectionScan"};

//
// Stage-specific tests.
//

class QueryStageCollectionScanTest : public unittest::Test {
public:
    QueryStageCollectionScanTest() : _client(&_opCtx) {
        dbtests::WriteContextForTests ctx(&_opCtx, nss.ns());

        for (int i = 0; i < numObj(); ++i) {
            BSONObjBuilder bob;
            bob.append("foo", i);
            _client.insert(nss.ns(), bob.obj());
        }
    }

    virtual ~QueryStageCollectionScanTest() {
        dbtests::WriteContextForTests ctx(&_opCtx, nss.ns());
        _client.dropCollection(nss.ns());
    }

    void remove(const BSONObj& obj) {
        _client.remove(nss.ns(), obj);
    }

    int countResults(CollectionScanParams::Direction direction, const BSONObj& filterObj) {
        AutoGetCollectionForReadCommand collection(&_opCtx, nss);

        // Configure the scan.
        CollectionScanParams params;
        params.direction = direction;
        params.tailable = false;

        // Make the filter.
        StatusWithMatchExpression statusWithMatcher =
            MatchExpressionParser::parse(filterObj, _expCtx);
        verify(statusWithMatcher.isOK());
        unique_ptr<MatchExpression> filterExpr = std::move(statusWithMatcher.getValue());

        // Make a scan and have the runner own it.
        unique_ptr<WorkingSet> ws = std::make_unique<WorkingSet>();
        unique_ptr<PlanStage> ps = std::make_unique<CollectionScan>(
            _expCtx.get(), collection.getCollection(), params, ws.get(), filterExpr.get());

        auto statusWithPlanExecutor =
            plan_executor_factory::make(_expCtx,
                                        std::move(ws),
                                        std::move(ps),
                                        &collection.getCollection(),
                                        PlanYieldPolicy::YieldPolicy::NO_YIELD,
                                        QueryPlannerParams::DEFAULT);
        ASSERT_OK(statusWithPlanExecutor.getStatus());
        auto exec = std::move(statusWithPlanExecutor.getValue());

        // Use the runner to count the number of objects scanned.
        int count = 0;
        PlanExecutor::ExecState state;
        for (BSONObj obj; PlanExecutor::ADVANCED == (state = exec->getNext(&obj, nullptr));) {
            ++count;
        }
        ASSERT_EQUALS(PlanExecutor::IS_EOF, state);
        return count;
    }

    void getRecordIds(const CollectionPtr& collection,
                      CollectionScanParams::Direction direction,
                      vector<RecordId>* out) {
        WorkingSet ws;

        CollectionScanParams params;
        params.direction = direction;
        params.tailable = false;

        unique_ptr<CollectionScan> scan(
            new CollectionScan(_expCtx.get(), collection, params, &ws, nullptr));
        while (!scan->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState state = scan->work(&id);
            if (PlanStage::ADVANCED == state) {
                WorkingSetMember* member = ws.get(id);
                verify(member->hasRecordId());
                out->push_back(member->recordId);
            }
        }
    }

    class ScopedCollectionDeleter {
    public:
        ScopedCollectionDeleter(OperationContext* opCtx, NamespaceString nss)
            : _opCtx(opCtx), _nss(nss) {}
        ~ScopedCollectionDeleter() {
            AutoGetDb autoDb(_opCtx, _nss.dbName(), MODE_IX);
            if (!autoDb.getDb())
                return;

            AutoGetCollection autoColl(_opCtx, _nss, MODE_X);
            if (!autoColl.getCollection())
                return;

            WriteUnitOfWork wuow(_opCtx);
            ASSERT_OK(autoDb.getDb()->dropCollection(_opCtx, _nss));
            wuow.commit();
        }
        ScopedCollectionDeleter(const ScopedCollectionDeleter&& other) = delete;

    private:
        OperationContext* _opCtx;
        NamespaceString _nss;
    };

    ScopedCollectionDeleter createClusteredCollection(const NamespaceString& ns,
                                                      bool prePopulate = true) {
        AutoGetCollection autoColl(&_opCtx, ns, MODE_IX);

        {
            auto db = autoColl.ensureDbExists(&_opCtx);

            WriteUnitOfWork wuow(&_opCtx);
            CollectionOptions collOptions;
            collOptions.clusteredIndex = clustered_util::makeDefaultClusteredIdIndex();
            const bool createIdIndex = false;
            db->createCollection(&_opCtx, ns, collOptions, createIdIndex);
            wuow.commit();
        }

        if (prePopulate) {
            for (int i = 0; i < numObj(); ++i) {
                _client.insert(ns.ns(), BSON("foo" << i));
            }
        }

        return {&_opCtx, ns};
    }

    void insertDocument(const NamespaceString& ns, const BSONObj& doc) {
        _client.insert(ns.ns(), doc);
    }

    void insertDocuments(const NamespaceString& ns, const vector<BSONObj>& docs, bool ordered) {
        _client.insert(ns.ns(), docs, ordered);
    }

    // Returns the recordId generated by doc, assuming doc takes the shape of {<cluster key> :
    // <value>};
    RecordIdBound getRecordIdForClusteredDoc(const BSONObj& doc) {
        return RecordIdBound(record_id_helpers::keyForElem(doc.firstElement()));
    }

    // Performs a bounded collection scan from 'minRecord' to 'maxRecord' in the specified
    // 'direction'. Asserts that the collection scan retrieves 'expectedNumMatches' documents.
    void runClusteredCollScan(const NamespaceString& ns,
                              CollectionScanParams::Direction direction,
                              RecordId minRecord,
                              RecordId maxRecord,
                              int expectedNumMatches) {
        AutoGetCollectionForRead autoColl(&_opCtx, ns);

        const CollectionPtr& coll = autoColl.getCollection();
        ASSERT(coll->isClustered());

        // Configure the scan.
        CollectionScanParams params;
        params.tailable = false;
        params.direction = direction;
        params.minRecord = RecordIdBound(minRecord);
        params.maxRecord = RecordIdBound(maxRecord);

        WorkingSet ws;
        auto scan = std::make_unique<CollectionScan>(_expCtx.get(), coll, params, &ws, nullptr);

        int count = 0;
        while (!scan->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState state = scan->work(&id);
            if (PlanStage::ADVANCED == state) {
                WorkingSetMember* member = ws.get(id);
                ASSERT(member->hasRecordId());
                ASSERT(member->hasObj());

                ASSERT_GTE(member->recordId, minRecord);
                ASSERT_LTE(member->recordId, maxRecord);

                count++;
            }
        }

        ASSERT_EQ(count, expectedNumMatches);
    }

    // Like runClusteredCollScan, but takes a CollectionScanParams:ScanBoundInclusion and
    // a vector with the expectedResults in order.
    void runClusteredCollScanAndAssertContents(
        const NamespaceString& ns,
        CollectionScanParams::Direction direction,
        boost::optional<RecordIdBound> minRecord,
        boost::optional<RecordIdBound> maxRecord,
        CollectionScanParams::ScanBoundInclusion boundInclusion,
        const vector<BSONObj>& expectedResults,
        const MatchExpression* filter = nullptr) {

        AutoGetCollectionForRead autoColl(&_opCtx, ns);

        const CollectionPtr& coll = autoColl.getCollection();
        ASSERT(coll->isClustered());

        CollectionScanParams params;
        params.tailable = false;
        params.minRecord = minRecord;
        params.maxRecord = maxRecord;
        params.direction = direction;
        params.boundInclusion = boundInclusion;

        WorkingSet ws;
        auto scan = std::make_unique<CollectionScan>(_expCtx.get(), coll, params, &ws, filter);

        int idx = 0;
        while (!scan->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState state = scan->work(&id);
            if (PlanStage::ADVANCED == state) {
                WorkingSetMember* member = ws.get(id);
                ASSERT(member->hasRecordId());
                ASSERT(member->hasObj());

                ASSERT_BSONOBJ_EQ(member->doc.value().toBson(), expectedResults[idx]);
                idx++;
            }
        }

        // When the scan is EOF, we expect the index to move past the last element of the
        // expectedResults.
        ASSERT_EQ(idx, expectedResults.size());
    }

    static int numObj() {
        return 50;
    }

protected:
    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_txnPtr;

    boost::intrusive_ptr<ExpressionContext> _expCtx =
        make_intrusive<ExpressionContext>(&_opCtx, nullptr, nss);

private:
    DBDirectClient _client;
};


// Go forwards, get everything.
TEST_F(QueryStageCollectionScanTest, QueryStageCollscanBasicForward) {
    ASSERT_EQUALS(numObj(), countResults(CollectionScanParams::FORWARD, BSONObj()));
}

// Go backwards, get everything.
TEST_F(QueryStageCollectionScanTest, QueryStageCollscanBasicBackward) {
    ASSERT_EQUALS(numObj(), countResults(CollectionScanParams::BACKWARD, BSONObj()));
}

// Go forwards and match half the docs.
TEST_F(QueryStageCollectionScanTest, QueryStageCollscanBasicForwardWithMatch) {
    BSONObj obj = BSON("foo" << BSON("$lt" << 25));
    ASSERT_EQUALS(25, countResults(CollectionScanParams::FORWARD, obj));
}

// Go backwards and match half the docs.
TEST_F(QueryStageCollectionScanTest, QueryStageCollscanBasicBackwardWithMatch) {
    BSONObj obj = BSON("foo" << BSON("$lt" << 25));
    ASSERT_EQUALS(25, countResults(CollectionScanParams::BACKWARD, obj));
}

// Get objects in the order we inserted them.
TEST_F(QueryStageCollectionScanTest, QueryStageCollscanObjectsInOrderForward) {
    AutoGetCollectionForReadCommand collection(&_opCtx, nss);

    // Configure the scan.
    CollectionScanParams params;
    params.direction = CollectionScanParams::FORWARD;
    params.tailable = false;

    // Make a scan and have the runner own it.
    unique_ptr<WorkingSet> ws = std::make_unique<WorkingSet>();
    unique_ptr<PlanStage> ps = std::make_unique<CollectionScan>(
        _expCtx.get(), collection.getCollection(), params, ws.get(), nullptr);

    auto statusWithPlanExecutor =
        plan_executor_factory::make(_expCtx,
                                    std::move(ws),
                                    std::move(ps),
                                    &collection.getCollection(),
                                    PlanYieldPolicy::YieldPolicy::NO_YIELD,
                                    QueryPlannerParams::DEFAULT);
    ASSERT_OK(statusWithPlanExecutor.getStatus());
    auto exec = std::move(statusWithPlanExecutor.getValue());

    int count = 0;
    PlanExecutor::ExecState state;
    for (BSONObj obj; PlanExecutor::ADVANCED == (state = exec->getNext(&obj, nullptr));) {
        // Make sure we get the objects in the order we want
        ASSERT_EQUALS(count, obj["foo"].numberInt());
        ++count;
    }
    ASSERT_EQUALS(PlanExecutor::IS_EOF, state);
    ASSERT_EQUALS(numObj(), count);
}

// Get objects in the reverse order we inserted them when we go backwards.
TEST_F(QueryStageCollectionScanTest, QueryStageCollscanObjectsInOrderBackward) {
    AutoGetCollectionForReadCommand collection(&_opCtx, nss);

    CollectionScanParams params;
    params.direction = CollectionScanParams::BACKWARD;
    params.tailable = false;

    unique_ptr<WorkingSet> ws = std::make_unique<WorkingSet>();
    unique_ptr<PlanStage> ps = std::make_unique<CollectionScan>(
        _expCtx.get(), collection.getCollection(), params, ws.get(), nullptr);

    auto statusWithPlanExecutor =
        plan_executor_factory::make(_expCtx,
                                    std::move(ws),
                                    std::move(ps),
                                    &collection.getCollection(),
                                    PlanYieldPolicy::YieldPolicy::NO_YIELD,
                                    QueryPlannerParams::DEFAULT);
    ASSERT_OK(statusWithPlanExecutor.getStatus());
    auto exec = std::move(statusWithPlanExecutor.getValue());

    int count = 0;
    PlanExecutor::ExecState state;
    for (BSONObj obj; PlanExecutor::ADVANCED == (state = exec->getNext(&obj, nullptr));) {
        ++count;
        ASSERT_EQUALS(numObj() - count, obj["foo"].numberInt());
    }
    ASSERT_EQUALS(PlanExecutor::IS_EOF, state);
    ASSERT_EQUALS(numObj(), count);
}

// Scan through half the objects, delete the one we're about to fetch, then expect to get the "next"
// object we would have gotten after that.
TEST_F(QueryStageCollectionScanTest, QueryStageCollscanDeleteUpcomingObject) {
    dbtests::WriteContextForTests ctx(&_opCtx, nss.ns());

    const CollectionPtr& coll = ctx.getCollection();

    // Get the RecordIds that would be returned by an in-order scan.
    vector<RecordId> recordIds;
    getRecordIds(coll, CollectionScanParams::FORWARD, &recordIds);

    // Configure the scan.
    CollectionScanParams params;
    params.direction = CollectionScanParams::FORWARD;
    params.tailable = false;

    WorkingSet ws;
    unique_ptr<PlanStage> scan(new CollectionScan(_expCtx.get(), coll, params, &ws, nullptr));

    int count = 0;
    while (count < 10) {
        WorkingSetID id = WorkingSet::INVALID_ID;
        PlanStage::StageState state = scan->work(&id);
        if (PlanStage::ADVANCED == state) {
            WorkingSetMember* member = ws.get(id);
            ASSERT_EQUALS(coll->docFor(&_opCtx, recordIds[count]).value()["foo"].numberInt(),
                          member->doc.value()["foo"].getInt());
            ++count;
        }
    }

    // Remove recordIds[count].
    scan->saveState();
    remove(coll->docFor(&_opCtx, recordIds[count]).value());
    scan->restoreState(&coll);

    // Skip over recordIds[count].
    ++count;

    // Expect the rest.
    while (!scan->isEOF()) {
        WorkingSetID id = WorkingSet::INVALID_ID;
        PlanStage::StageState state = scan->work(&id);
        if (PlanStage::ADVANCED == state) {
            WorkingSetMember* member = ws.get(id);
            ASSERT_EQUALS(coll->docFor(&_opCtx, recordIds[count]).value()["foo"].numberInt(),
                          member->doc.value()["foo"].getInt());
            ++count;
        }
    }

    ASSERT_EQUALS(numObj(), count);
}

// Scan through half the objects, delete the one we're about to fetch, then expect to get the "next"
// object we would have gotten after that.  But, do it in reverse!
TEST_F(QueryStageCollectionScanTest, QueryStageCollscanDeleteUpcomingObjectBackward) {
    dbtests::WriteContextForTests ctx(&_opCtx, nss.ns());
    const CollectionPtr& coll = ctx.getCollection();

    // Get the RecordIds that would be returned by an in-order scan.
    vector<RecordId> recordIds;
    getRecordIds(coll, CollectionScanParams::BACKWARD, &recordIds);

    // Configure the scan.
    CollectionScanParams params;
    params.direction = CollectionScanParams::BACKWARD;
    params.tailable = false;

    WorkingSet ws;
    unique_ptr<PlanStage> scan(new CollectionScan(_expCtx.get(), coll, params, &ws, nullptr));

    int count = 0;
    while (count < 10) {
        WorkingSetID id = WorkingSet::INVALID_ID;
        PlanStage::StageState state = scan->work(&id);
        if (PlanStage::ADVANCED == state) {
            WorkingSetMember* member = ws.get(id);
            ASSERT_EQUALS(coll->docFor(&_opCtx, recordIds[count]).value()["foo"].numberInt(),
                          member->doc.value()["foo"].getInt());
            ++count;
        }
    }

    // Remove recordIds[count].
    scan->saveState();
    remove(coll->docFor(&_opCtx, recordIds[count]).value());
    scan->restoreState(&coll);

    // Skip over recordIds[count].
    ++count;

    // Expect the rest.
    while (!scan->isEOF()) {
        WorkingSetID id = WorkingSet::INVALID_ID;
        PlanStage::StageState state = scan->work(&id);
        if (PlanStage::ADVANCED == state) {
            WorkingSetMember* member = ws.get(id);
            ASSERT_EQUALS(coll->docFor(&_opCtx, recordIds[count]).value()["foo"].numberInt(),
                          member->doc.value()["foo"].getInt());
            ++count;
        }
    }

    ASSERT_EQUALS(numObj(), count);
}

// Verify that successfully seeking to the resumeAfterRecordId returns PlanStage::NEED_TIME and
// that we can complete the collection scan afterwards.
TEST_F(QueryStageCollectionScanTest, QueryTestCollscanResumeAfterRecordIdSeekSuccess) {
    AutoGetCollectionForReadCommand collection(&_opCtx, nss);

    // Get the RecordIds that would be returned by an in-order scan.
    vector<RecordId> recordIds;
    getRecordIds(collection.getCollection(), CollectionScanParams::FORWARD, &recordIds);

    // We will resume the collection scan this many results in.
    auto offset = 10;

    // Configure the scan.
    CollectionScanParams params;
    params.direction = CollectionScanParams::FORWARD;

    // Pick a recordId that is known to be in the collection.
    params.resumeAfterRecordId = recordIds[offset - 1];

    // Create plan stage.
    unique_ptr<WorkingSet> ws = std::make_unique<WorkingSet>();
    unique_ptr<PlanStage> ps = std::make_unique<CollectionScan>(
        _expCtx.get(), collection.getCollection(), params, ws.get(), nullptr);

    WorkingSetID id = WorkingSet::INVALID_ID;

    // Check that the resume succeeds in making the cursor.
    ASSERT_EQUALS(PlanStage::NEED_TIME, ps->work(&id));

    // Run the rest of the scan and verify the results.
    auto statusWithPlanExecutor =
        plan_executor_factory::make(_expCtx,
                                    std::move(ws),
                                    std::move(ps),
                                    &collection.getCollection(),
                                    PlanYieldPolicy::YieldPolicy::NO_YIELD,
                                    QueryPlannerParams::DEFAULT);
    ASSERT_OK(statusWithPlanExecutor.getStatus());
    auto exec = std::move(statusWithPlanExecutor.getValue());

    int count = 0;
    PlanExecutor::ExecState state;
    for (BSONObj obj; PlanExecutor::ADVANCED == (state = exec->getNext(&obj, nullptr));) {
        // Make sure we get the objects in the order we want.
        ASSERT_EQUALS(count + offset, obj["foo"].numberInt());
        ++count;
    }
    ASSERT_EQUALS(PlanExecutor::IS_EOF, state);
    ASSERT_EQUALS(numObj() - offset, count);
}

// Verify that if we fail to seek to the resumeAfterRecordId, the plan stage fails.
TEST_F(QueryStageCollectionScanTest, QueryTestCollscanResumeAfterRecordIdSeekFailure) {
    dbtests::WriteContextForTests ctx(&_opCtx, nss.ns());
    auto coll = ctx.getCollection();

    // Get the RecordIds that would be returned by an in-order scan.
    vector<RecordId> recordIds;
    getRecordIds(coll, CollectionScanParams::FORWARD, &recordIds);

    // We will resume the collection scan this many results in.
    auto offset = 10;

    // Configure the scan.
    CollectionScanParams params;
    params.direction = CollectionScanParams::FORWARD;

    // Pick a recordId that is known to be in the collection and then delete it, so that we can
    // guarantee it does not exist.
    auto recordId = recordIds[offset - 1];
    remove(coll->docFor(&_opCtx, recordId).value());
    params.resumeAfterRecordId = recordId;

    // Create plan stage.
    unique_ptr<WorkingSet> ws = std::make_unique<WorkingSet>();
    unique_ptr<PlanStage> ps =
        std::make_unique<CollectionScan>(_expCtx.get(), coll, params, ws.get(), nullptr);

    WorkingSetID id = WorkingSet::INVALID_ID;

    // Check that failed seek causes the entire resume to fail.
    ASSERT_THROWS_CODE(ps->work(&id), DBException, ErrorCodes::KeyNotFound);
}

TEST_F(QueryStageCollectionScanTest, QueryTestCollscanClusteredMinMax) {
    auto ns = NamespaceString("a.b");
    auto collDeleter = createClusteredCollection(ns);
    AutoGetCollectionForRead autoColl(&_opCtx, ns);
    const CollectionPtr& coll = autoColl.getCollection();

    ASSERT(coll->isClustered());

    // Get the RecordIds that would be returned by an in-order scan.
    vector<RecordId> recordIds;
    getRecordIds(coll, CollectionScanParams::FORWARD, &recordIds);
    ASSERT(recordIds.size());

    // Configure the scan.
    CollectionScanParams params;
    params.direction = CollectionScanParams::FORWARD;
    params.tailable = false;
    params.minRecord = RecordIdBound(recordIds[0]);
    params.maxRecord = RecordIdBound(recordIds[recordIds.size() - 1]);

    WorkingSet ws;
    auto scan = std::make_unique<CollectionScan>(_expCtx.get(), coll, params, &ws, nullptr);

    // Expect to see all RecordIds.
    int count = 0;
    while (!scan->isEOF()) {
        WorkingSetID id = WorkingSet::INVALID_ID;
        PlanStage::StageState state = scan->work(&id);
        if (PlanStage::ADVANCED == state) {
            WorkingSetMember* member = ws.get(id);
            ASSERT(member->hasRecordId());
            ASSERT(member->hasObj());
            ASSERT_EQ(member->recordId, recordIds[count]);
            ASSERT_EQUALS(coll->docFor(&_opCtx, recordIds[count]).value()["foo"].numberInt(),
                          member->doc.value()["foo"].getInt());
            count++;
        }
    }

    ASSERT_EQ(count, recordIds.size());
}

// Tests a collection scan with bounds generated from type 'date', on a collection with all entries
// generated from type 'objectId', exludes all entries.
TEST_F(QueryStageCollectionScanTest, QueryTestCollscanClusteredMinMaxBoundsDateTypeNoMatches) {
    const std::vector<CollectionScanParams::Direction> collScanDirections{
        CollectionScanParams::FORWARD, CollectionScanParams::BACKWARD};

    for (const auto direction : collScanDirections) {
        LOGV2(6028701,
              "Running clustered collection scan test case",
              "scanDirection"_attr =
                  (direction == CollectionScanParams::FORWARD ? "FORWARD" : "BACKWARD"));

        auto ns = NamespaceString("a.b");

        // Create a clustered collection pre-populated with RecordIds generated from type
        // 'objectId'.
        auto scopedCollectionDeleter = createClusteredCollection(ns);

        // Use bounds that restrict the scan to RecordIds generated from type 'date'.
        auto minRecord = record_id_helpers::keyForDate(Date_t::min());
        auto maxRecord = record_id_helpers::keyForDate(Date_t::max());

        // The collection has no records generated with type 'date'. There should be 0 matches.
        runClusteredCollScan(ns, direction, minRecord, maxRecord, 0);
    }
}

// Tests that if the bounds are generated from type 'date', only RecordIds generated with type
// 'date' are included in the results.
TEST_F(QueryStageCollectionScanTest, QueryTestCollscanClusteredMinMaxDateTypeMatches) {
    const std::vector<CollectionScanParams::Direction> collScanDirections{
        CollectionScanParams::FORWARD, CollectionScanParams::BACKWARD};

    for (const auto direction : collScanDirections) {
        LOGV2(6028702,
              "Running clustered collection scan test case",
              "scanDirection"_attr =
                  (direction == CollectionScanParams::FORWARD ? "FORWARD" : "BACKWARD"));

        auto ns = NamespaceString("a.b");

        // Create a clustered collection pre-populated with RecordIds generated from type
        // 'objectId'.
        auto scopedCollectionDeleter = createClusteredCollection(ns);

        auto numDateDocs = 5;

        // Insert documents that generate a RecordId with type 'date'.
        Date_t now = Date_t::now();
        for (int i = 0; i < numDateDocs; i++) {
            insertDocument(ns, BSON("_id" << now - Milliseconds(i)));
        }

        // Generate bounds from type 'date'.
        auto minRecord = record_id_helpers::keyForDate(Date_t::min());
        auto maxRecord = record_id_helpers::keyForDate(Date_t::max());

        // The collection contains RecordIds generated from both type 'objectId' and 'date'. Only
        // RecordIds that match the bound type should be included in the scan.
        runClusteredCollScan(ns, direction, minRecord, maxRecord, numDateDocs);
    }
}

TEST_F(QueryStageCollectionScanTest, QueryTestCollscanClusteredIgnoreNumericRecordIds) {
    const std::vector<CollectionScanParams::Direction> collScanDirections{
        CollectionScanParams::FORWARD, CollectionScanParams::BACKWARD};

    for (const auto direction : collScanDirections) {
        LOGV2(6028703,
              "Running clustered collection scan test case",
              "scanDirection"_attr =
                  (direction == CollectionScanParams::FORWARD ? "FORWARD" : "BACKWARD"));
        auto ns = NamespaceString("a.b");
        auto scopedCollectionDeleter = createClusteredCollection(ns, false /* prePopulate */);

        int numOIDDocs = 20;
        // Insert documents with default '_id' values of type 'objectId' used to generate their
        // RecordIds.
        for (int i = 0; i < numOIDDocs; i++) {
            insertDocument(ns, BSON("foo" << i));
        }

        // Insert documents that generate 'numeric' typed RecordIds.
        auto numNumericDocs = 10;
        for (int i = 0; i < numNumericDocs; i++) {
            insertDocument(ns, BSON("_id" << i));
        }

        // Use bounds that will include every 'objectId' typed record.
        auto minRecord = record_id_helpers::keyForOID(OID());
        auto maxRecord = record_id_helpers::keyForOID(OID::max());

        // Only records generated from type 'objectId' should result from the scan.
        runClusteredCollScan(ns, direction, minRecord, maxRecord, numOIDDocs);
    }
}

// Test exclusive filters work for date typed collection scan bounds.
TEST_F(QueryStageCollectionScanTest, QueryTestCollscanClusteredMinMaxDateExclusiveFilter) {
    const std::vector<CollectionScanParams::Direction> collScanDirections{
        CollectionScanParams::FORWARD, CollectionScanParams::BACKWARD};

    for (const auto direction : collScanDirections) {
        LOGV2(6028704,
              "Running clustered collection scan test case",
              "scanDirection"_attr =
                  (direction == CollectionScanParams::FORWARD ? "FORWARD" : "BACKWARD"));

        auto ns = NamespaceString("a.b");

        auto scopedCollectionDeleter = createClusteredCollection(ns, false /* prePopulate */);

        Lock::GlobalLock lk{&_opCtx, MODE_IX};  // avoid global lock upgrade during insertion
        AutoGetCollectionForRead autoColl(&_opCtx, ns);
        const CollectionPtr& coll = autoColl.getCollection();

        Date_t maxDate = Date_t::now();
        Date_t middleDate = maxDate - Milliseconds(1);
        Date_t minDate = middleDate - Milliseconds(1);
        std::vector<BSONObj> dateDocuments = {
            BSON("_id" << minDate), BSON("_id" << middleDate), BSON("_id" << maxDate)};
        for (auto doc : dateDocuments) {
            insertDocument(ns, doc);
        }

        CollectionScanParams params;
        params.tailable = false;
        params.direction = direction;

        params.minRecord = RecordIdBound(record_id_helpers::keyForDate(minDate));
        params.maxRecord = RecordIdBound(record_id_helpers::keyForDate(maxDate));

        // Exclude all but the record with _id 'middleDate' from the scan.
        StatusWithMatchExpression swMatch = MatchExpressionParser::parse(
            BSON("_id" << BSON("$gt" << minDate << "$lt" << maxDate)), _expCtx.get());

        ASSERT_OK(swMatch.getStatus());
        auto filter = std::move(swMatch.getValue());

        WorkingSet ws;
        auto scan =
            std::make_unique<CollectionScan>(_expCtx.get(), coll, params, &ws, filter.get());

        int count = 0;
        while (!scan->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState state = scan->work(&id);
            if (PlanStage::ADVANCED == state) {
                WorkingSetMember* member = ws.get(id);
                ASSERT(member->hasRecordId());
                ASSERT(member->hasObj());

                ASSERT_NOT_EQUALS(member->recordId, record_id_helpers::keyForDate(maxDate));
                ASSERT_NOT_EQUALS(member->recordId, record_id_helpers::keyForDate(minDate));
                count++;
            }
        }

        // Verify the min and max bounds are excluded.
        ASSERT_EQ(count, dateDocuments.size() - 2);
    }
}

TEST_F(QueryStageCollectionScanTest, QueryTestCollscanClusteredReverse) {
    auto ns = NamespaceString("a.b");
    auto collDeleter = createClusteredCollection(ns);
    AutoGetCollectionForRead autoColl(&_opCtx, ns);
    const CollectionPtr& coll = autoColl.getCollection();

    ASSERT(coll->isClustered());

    // Get the RecordIds that would be returned by a backwards scan.
    vector<RecordId> recordIds;
    getRecordIds(coll, CollectionScanParams::BACKWARD, &recordIds);
    ASSERT(recordIds.size());

    // Configure the scan.
    CollectionScanParams params;
    params.direction = CollectionScanParams::BACKWARD;
    params.tailable = false;
    // The last entry in recordIds is the lowest record in the collection and the first entry is the
    // highest.
    params.minRecord = RecordIdBound(recordIds[recordIds.size() - 1]);
    params.maxRecord = RecordIdBound(recordIds[0]);

    WorkingSet ws;
    auto scan = std::make_unique<CollectionScan>(_expCtx.get(), coll, params, &ws, nullptr);

    // Expect to see all RecordIds.
    int count = 0;
    while (!scan->isEOF()) {
        WorkingSetID id = WorkingSet::INVALID_ID;
        PlanStage::StageState state = scan->work(&id);
        if (PlanStage::ADVANCED == state) {
            WorkingSetMember* member = ws.get(id);
            ASSERT(member->hasRecordId());
            ASSERT(member->hasObj());
            ASSERT_EQ(member->recordId, recordIds[count]);
            ASSERT_EQUALS(coll->docFor(&_opCtx, recordIds[count]).value()["foo"].numberInt(),
                          member->doc.value()["foo"].getInt());
            count++;
        }
    }

    ASSERT_EQ(count, recordIds.size());
}

TEST_F(QueryStageCollectionScanTest, QueryTestCollscanClusteredMinMaxFullObjectIdRange) {
    auto ns = NamespaceString("a.b");
    auto collDeleter = createClusteredCollection(ns);
    AutoGetCollectionForRead autoColl(&_opCtx, ns);
    const CollectionPtr& coll = autoColl.getCollection();

    ASSERT(coll->isClustered());

    // Get the RecordIds that would be returned by an in-order scan.
    vector<RecordId> recordIds;
    getRecordIds(coll, CollectionScanParams::FORWARD, &recordIds);
    ASSERT(recordIds.size());

    // Configure the scan.
    CollectionScanParams params;
    params.direction = CollectionScanParams::FORWARD;
    params.tailable = false;

    // Expect to see all records.
    params.minRecord = RecordIdBound(record_id_helpers::keyForOID(OID()));
    params.maxRecord = RecordIdBound(record_id_helpers::keyForOID(OID::max()));

    WorkingSet ws;
    auto scan = std::make_unique<CollectionScan>(_expCtx.get(), coll, params, &ws, nullptr);

    // Expect to see all RecordIds.
    int count = 0;
    while (!scan->isEOF()) {
        WorkingSetID id = WorkingSet::INVALID_ID;
        PlanStage::StageState state = scan->work(&id);
        if (PlanStage::ADVANCED == state) {
            WorkingSetMember* member = ws.get(id);
            ASSERT(member->hasRecordId());
            ASSERT(member->hasObj());
            ASSERT_EQ(member->recordId, recordIds[count]);
            ASSERT_EQUALS(coll->docFor(&_opCtx, recordIds[count]).value()["foo"].numberInt(),
                          member->doc.value()["foo"].getInt());
            count++;
        }
    }

    ASSERT_EQ(count, recordIds.size());
}

TEST_F(QueryStageCollectionScanTest, QueryTestCollscanClusteredInnerRange) {
    auto ns = NamespaceString("a.b");
    auto collDeleter = createClusteredCollection(ns);
    AutoGetCollectionForRead autoColl(&_opCtx, ns);
    const CollectionPtr& coll = autoColl.getCollection();

    ASSERT(coll->isClustered());

    // Get the RecordIds that would be returned by an in-order scan.
    vector<RecordId> recordIds;
    getRecordIds(coll, CollectionScanParams::FORWARD, &recordIds);
    ASSERT(recordIds.size());

    // Configure the scan.
    CollectionScanParams params;
    params.direction = CollectionScanParams::FORWARD;
    params.tailable = false;

    const int startOffset = 10;
    const int endOffset = 20;
    ASSERT_LT(startOffset, recordIds.size());
    ASSERT_LT(endOffset, recordIds.size());

    params.minRecord = RecordIdBound(recordIds[startOffset]);
    params.maxRecord = RecordIdBound(recordIds[endOffset]);

    WorkingSet ws;
    auto scan = std::make_unique<CollectionScan>(_expCtx.get(), coll, params, &ws, nullptr);

    int count = 0;
    while (!scan->isEOF()) {
        WorkingSetID id = WorkingSet::INVALID_ID;
        PlanStage::StageState state = scan->work(&id);
        if (PlanStage::ADVANCED == state) {
            WorkingSetMember* member = ws.get(id);
            ASSERT(member->hasRecordId());
            ASSERT(member->hasObj());
            int i = startOffset + count;
            ASSERT_EQ(member->recordId, recordIds[i]);
            ASSERT_EQUALS(coll->docFor(&_opCtx, recordIds[i]).value()["foo"].numberInt(),
                          member->doc.value()["foo"].getInt());
            count++;
        }
    }

    // There are 11 records between 10 and 20 inclusive
    ASSERT_EQ(count, 1 + endOffset - startOffset);
}

TEST_F(QueryStageCollectionScanTest, QueryTestCollscanClusteredInnerRangeExclusiveFilter) {
    auto ns = NamespaceString("a.b");
    auto collDeleter = createClusteredCollection(ns);
    AutoGetCollectionForRead autoColl(&_opCtx, ns);
    const CollectionPtr& coll = autoColl.getCollection();

    ASSERT(coll->isClustered());

    // Get the RecordIds that would be returned by an in-order scan.
    vector<RecordId> recordIds;
    getRecordIds(coll, CollectionScanParams::FORWARD, &recordIds);
    ASSERT(recordIds.size());

    // Configure the scan.
    CollectionScanParams params;
    params.direction = CollectionScanParams::FORWARD;
    params.tailable = false;

    const int startOffset = 10;
    const int endOffset = 20;
    ASSERT_LT(startOffset, recordIds.size());
    ASSERT_LT(endOffset, recordIds.size());

    params.minRecord = RecordIdBound(recordIds[startOffset]);
    params.maxRecord = RecordIdBound(recordIds[endOffset]);

    // Provide RecordId bounds with exclusive filters.
    StatusWithMatchExpression swMatch = MatchExpressionParser::parse(
        fromjson(fmt::sprintf("{_id: {$gt: ObjectId('%s'), $lt: ObjectId('%s')}}",
                              record_id_helpers::toBSONAs(params.minRecord->recordId(), "")
                                  .firstElement()
                                  .OID()
                                  .toString(),
                              record_id_helpers::toBSONAs(params.maxRecord->recordId(), "")
                                  .firstElement()
                                  .OID()
                                  .toString())),
        _expCtx.get());
    ASSERT_OK(swMatch.getStatus());
    auto filter = std::move(swMatch.getValue());

    WorkingSet ws;
    auto scan = std::make_unique<CollectionScan>(_expCtx.get(), coll, params, &ws, filter.get());

    // The expected range should not include the first or last records.
    std::vector<RecordId> expectedIds{recordIds.begin() + startOffset + 1,
                                      recordIds.begin() + endOffset};
    int count = 0;
    while (!scan->isEOF()) {
        WorkingSetID id = WorkingSet::INVALID_ID;
        PlanStage::StageState state = scan->work(&id);
        if (PlanStage::ADVANCED == state) {
            WorkingSetMember* member = ws.get(id);
            ASSERT(member->hasRecordId());
            ASSERT(member->hasObj());
            ASSERT_EQ(member->recordId, expectedIds[count]);
            ASSERT_EQUALS(coll->docFor(&_opCtx, expectedIds[count]).value()["foo"].numberInt(),
                          member->doc.value()["foo"].getInt());
            count++;
        }
    }

    ASSERT_EQ(count, expectedIds.size());
}

TEST_F(QueryStageCollectionScanTest, QueryTestCollscanClusteredInnerRangeExclusiveFilterReverse) {
    auto ns = NamespaceString("a.b");
    auto collDeleter = createClusteredCollection(ns);
    AutoGetCollectionForRead autoColl(&_opCtx, ns);
    const CollectionPtr& coll = autoColl.getCollection();

    ASSERT(coll->isClustered());

    // Get the RecordIds that would be returned by a reverse scan.
    vector<RecordId> recordIds;
    getRecordIds(coll, CollectionScanParams::BACKWARD, &recordIds);
    ASSERT(recordIds.size());

    // Configure the scan.
    CollectionScanParams params;
    params.direction = CollectionScanParams::BACKWARD;
    params.tailable = false;

    const int startOffset = 10;
    const int endOffset = 20;
    ASSERT_LT(startOffset, recordIds.size());
    ASSERT_LT(endOffset, recordIds.size());

    // The last entry in recordIds is the lowest record in the collection and the first entry is the
    // highest.
    params.minRecord = RecordIdBound(recordIds[endOffset]);
    params.maxRecord = RecordIdBound(recordIds[startOffset]);

    // Provide RecordId bounds with exclusive filters.
    StatusWithMatchExpression swMatch = MatchExpressionParser::parse(
        fromjson(fmt::sprintf("{_id: {$gt: ObjectId('%s'), $lt: ObjectId('%s')}}",
                              record_id_helpers::toBSONAs(params.minRecord->recordId(), "")
                                  .firstElement()
                                  .OID()
                                  .toString(),
                              record_id_helpers::toBSONAs(params.maxRecord->recordId(), "")
                                  .firstElement()
                                  .OID()
                                  .toString())),
        _expCtx.get());
    ASSERT_OK(swMatch.getStatus());
    auto filter = std::move(swMatch.getValue());

    WorkingSet ws;
    auto scan = std::make_unique<CollectionScan>(_expCtx.get(), coll, params, &ws, filter.get());

    // The expected range should not include the first or last records.
    std::vector<RecordId> expectedIds{recordIds.begin() + startOffset + 1,
                                      recordIds.begin() + endOffset};
    int count = 0;
    while (!scan->isEOF()) {
        WorkingSetID id = WorkingSet::INVALID_ID;
        PlanStage::StageState state = scan->work(&id);
        if (PlanStage::ADVANCED == state) {
            WorkingSetMember* member = ws.get(id);
            ASSERT(member->hasRecordId());
            ASSERT(member->hasObj());
            ASSERT_EQ(member->recordId, expectedIds[count]);
            ASSERT_EQUALS(coll->docFor(&_opCtx, expectedIds[count]).value()["foo"].numberInt(),
                          member->doc.value()["foo"].getInt());
            count++;
        }
    }

    ASSERT_EQ(count, expectedIds.size());
}

// Test clustered collection scan with
// CollectionScanParams::ScanBoundInclusion::kIncludeStartRecordOnly.
TEST_F(QueryStageCollectionScanTest,
       QueryTestCollscanClusteredInclusionBoundIncludeStartRecordOnly) {
    const std::vector<CollectionScanParams::Direction> collScanDirections{
        CollectionScanParams::FORWARD, CollectionScanParams::BACKWARD};

    for (const auto direction : collScanDirections) {
        LOGV2(6125001,
              "Running clustered collection scan test case",
              "scanDirection"_attr =
                  (direction == CollectionScanParams::FORWARD ? "FORWARD" : "BACKWARD"));
        auto ns = NamespaceString("a.b");
        auto scopedCollectionDeleter = createClusteredCollection(ns, false /* prePopulate */);

        std::vector<BSONObj> docs{BSON("_id" << 0),
                                  BSON("_id" << 1),
                                  BSON("_id" << 2),
                                  BSON("_id" << 3),
                                  BSON("_id" << 4)};
        insertDocuments(ns, docs, true /** ordered **/);

        auto minRecord = getRecordIdForClusteredDoc(BSON("_id" << 0));
        auto maxRecord = getRecordIdForClusteredDoc(BSON("_id" << 4));

        std::vector<BSONObj> expectedResults{docs.begin(), docs.end()};
        if (direction == CollectionScanParams::BACKWARD) {
            std::reverse(expectedResults.begin(), expectedResults.end());
        }

        // Recall kIncludeStartRecordOnly means the last document in the scan range should be
        // excluded from the results.
        expectedResults.pop_back();

        runClusteredCollScanAndAssertContents(
            ns,
            direction,
            minRecord,
            maxRecord,
            CollectionScanParams::ScanBoundInclusion::kIncludeStartRecordOnly,
            expectedResults);
    }
}

// Test clustered collection scan with
// CollectionScanParams::ScanBoundInclusion::kIncludeEndRecordOnly.
TEST_F(QueryStageCollectionScanTest, QueryTestCollscanClusteredInclusionBoundIncludeEndRecordOnly) {
    const std::vector<CollectionScanParams::Direction> collScanDirections{
        CollectionScanParams::FORWARD, CollectionScanParams::BACKWARD};

    for (const auto direction : collScanDirections) {
        LOGV2(6125002,
              "Running clustered collection scan test case",
              "scanDirection"_attr =
                  (direction == CollectionScanParams::FORWARD ? "FORWARD" : "BACKWARD"));
        auto ns = NamespaceString("a.b");
        auto scopedCollectionDeleter = createClusteredCollection(ns, false /* prePopulate */);

        std::vector<BSONObj> docs{BSON("_id" << 0),
                                  BSON("_id" << 1),
                                  BSON("_id" << 2),
                                  BSON("_id" << 3),
                                  BSON("_id" << 4)};
        insertDocuments(ns, docs, true /** ordered **/);

        auto minRecord = getRecordIdForClusteredDoc(BSON("_id" << 0));
        auto maxRecord = getRecordIdForClusteredDoc(BSON("_id" << 4));

        std::vector<BSONObj> expectedResults{docs.begin(), docs.end()};
        if (direction == CollectionScanParams::BACKWARD) {
            std::reverse(expectedResults.begin(), expectedResults.end());
        }

        // Recall kIncludeEndRecordOnly means the start document in the scan range should be
        // excluded from the results.
        expectedResults.erase(expectedResults.begin());

        runClusteredCollScanAndAssertContents(
            ns,
            direction,
            minRecord,
            maxRecord,
            CollectionScanParams::ScanBoundInclusion::kIncludeEndRecordOnly,
            expectedResults);
    }
}

// Test clustered collection scan with
// CollectionScanParams::ScanBoundInclusion::kExcludeBothStartAndEndRecords.
TEST_F(QueryStageCollectionScanTest,
       QueryTestCollscanClusteredInclusionBoundExcludeBothStartAndEndRecords) {
    const std::vector<CollectionScanParams::Direction> collScanDirections{
        CollectionScanParams::FORWARD, CollectionScanParams::BACKWARD};

    for (const auto direction : collScanDirections) {
        LOGV2(6125003,
              "Running clustered collection scan test case",
              "scanDirection"_attr =
                  (direction == CollectionScanParams::FORWARD ? "FORWARD" : "BACKWARD"));
        auto ns = NamespaceString("a.b");
        auto scopedCollectionDeleter = createClusteredCollection(ns, false /* prePopulate */);

        std::vector<BSONObj> docs{BSON("_id" << 0),
                                  BSON("_id" << 1),
                                  BSON("_id" << 2),
                                  BSON("_id" << 3),
                                  BSON("_id" << 4)};
        insertDocuments(ns, docs, true /** ordered **/);

        auto minRecord = getRecordIdForClusteredDoc(BSON("_id" << 0));
        auto maxRecord = getRecordIdForClusteredDoc(BSON("_id" << 4));

        std::vector<BSONObj> expectedResults{docs.begin(), docs.end()};
        if (direction == CollectionScanParams::BACKWARD) {
            std::reverse(expectedResults.begin(), expectedResults.end());
        }

        // Exclude the records at both ends of the collection scan range.
        expectedResults.erase(expectedResults.begin());
        expectedResults.pop_back();

        runClusteredCollScanAndAssertContents(
            ns,
            direction,
            minRecord,
            maxRecord,
            CollectionScanParams::ScanBoundInclusion::kExcludeBothStartAndEndRecords,
            expectedResults);
    }
}

// Test clustered collection scan with CollectionScanParams::ScanInclusionBound such that no results
// match.
TEST_F(QueryStageCollectionScanTest, QueryTestCollscanClusteredInclusionBoundYieldsNoResult) {
    const std::vector<CollectionScanParams::Direction> collScanDirections{
        CollectionScanParams::FORWARD, CollectionScanParams::BACKWARD};

    for (const auto direction : collScanDirections) {
        LOGV2(6125004,
              "Running clustered collection scan test case",
              "scanDirection"_attr =
                  (direction == CollectionScanParams::FORWARD ? "FORWARD" : "BACKWARD"));
        auto ns = NamespaceString("a.b");
        auto scopedCollectionDeleter = createClusteredCollection(ns, false /* prePopulate */);

        std::vector<BSONObj> docs{BSON("_id" << 0),
                                  BSON("_id" << 1),
                                  BSON("_id" << 2),
                                  BSON("_id" << 3),
                                  BSON("_id" << 4)};
        insertDocuments(ns, docs, true /** ordered **/);

        auto minRecord = getRecordIdForClusteredDoc(BSON("_id" << 2));
        auto maxRecord = getRecordIdForClusteredDoc(BSON("_id" << 3));

        runClusteredCollScanAndAssertContents(
            ns,
            direction,
            minRecord,
            maxRecord,
            CollectionScanParams::ScanBoundInclusion::kExcludeBothStartAndEndRecords,
            {} /** expected results **/);
    }
}


// CollectionScanParams::ScanInclusionBound exclusions should take presidence over inclusive
// filtering.
TEST_F(QueryStageCollectionScanTest, QueryTestCollscanClusteredInclusionBoundsOverrideFilter) {
    const std::vector<CollectionScanParams::Direction> collScanDirections{
        CollectionScanParams::FORWARD, CollectionScanParams::BACKWARD};

    for (const auto direction : collScanDirections) {
        LOGV2(6028705,
              "Running clustered collection scan test case",
              "scanDirection"_attr =
                  (direction == CollectionScanParams::FORWARD ? "FORWARD" : "BACKWARD"));

        auto ns = NamespaceString("a.b");

        auto scopedCollectionDeleter = createClusteredCollection(ns, false /* prePopulate */);
        std::vector<BSONObj> docs{BSON("_id" << 0),
                                  BSON("_id" << 1),
                                  BSON("_id" << 2),
                                  BSON("_id" << 3),
                                  BSON("_id" << 4)};
        insertDocuments(ns, docs, true /** ordered **/);

        auto minRecord = getRecordIdForClusteredDoc(BSON("_id" << 0));
        auto maxRecord = getRecordIdForClusteredDoc(BSON("_id" << 4));

        std::vector<BSONObj> expectedResults{docs.begin(), docs.end()};
        if (direction == CollectionScanParams::BACKWARD) {
            std::reverse(expectedResults.begin(), expectedResults.end());
        }

        // Exclude the records at both ends of the collection scan range.
        expectedResults.erase(expectedResults.begin());
        expectedResults.pop_back();


        // Filtering includes the min and max records. However the ScanBoundInclusion should enforce
        // the bounds are not included in the results.
        StatusWithMatchExpression swMatch = MatchExpressionParser::parse(
            BSON("_id" << BSON("$gte" << 0 << "$lte" << 4)), _expCtx.get());

        ASSERT_OK(swMatch.getStatus());
        auto filter = std::move(swMatch.getValue());

        runClusteredCollScanAndAssertContents(
            ns,
            direction,
            minRecord,
            maxRecord,
            CollectionScanParams::ScanBoundInclusion::kExcludeBothStartAndEndRecords,
            expectedResults,
            filter.get());
    }
}

// Since the minRecord and maxRecord of a bounded collection scan are optional, a ScanBoundInclusion
// excluding a bound not defined should result in regular, inclusive behavior by default.
TEST_F(QueryStageCollectionScanTest, QueryTestCollscanClusteredInclusionBoundsHaveNoImpact) {
    const std::vector<CollectionScanParams::Direction> collScanDirections{
        CollectionScanParams::FORWARD, CollectionScanParams::BACKWARD};

    for (const auto direction : collScanDirections) {
        LOGV2(6028706,
              "Running clustered collection scan test case",
              "scanDirection"_attr =
                  (direction == CollectionScanParams::FORWARD ? "FORWARD" : "BACKWARD"));

        auto ns = NamespaceString("a.b");

        auto scopedCollectionDeleter = createClusteredCollection(ns, false /* prePopulate */);
        std::vector<BSONObj> docs{BSON("_id" << 0),
                                  BSON("_id" << 1),
                                  BSON("_id" << 2),
                                  BSON("_id" << 3),
                                  BSON("_id" << 4)};
        insertDocuments(ns, docs, true /** ordered **/);

        auto minRecord = boost::none;
        auto maxRecord = boost::none;

        std::vector<BSONObj> expectedResults{docs.begin(), docs.end()};
        if (direction == CollectionScanParams::BACKWARD) {
            std::reverse(expectedResults.begin(), expectedResults.end());
        }

        runClusteredCollScanAndAssertContents(
            ns,
            direction,
            minRecord,
            maxRecord,
            CollectionScanParams::ScanBoundInclusion::kExcludeBothStartAndEndRecords,
            expectedResults);
    }
}

}  // namespace query_stage_collection_scan
