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

#include <memory>

#include "mongo/client/dbclient_cursor.h"
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
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point_service.h"

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
        AutoGetCollectionForReadCommand ctx(&_opCtx, nss);
        auto collection = ctx.getCollection();

        // Configure the scan.
        CollectionScanParams params;
        params.direction = direction;
        params.tailable = false;

        // Make the filter.
        const CollatorInterface* collator = nullptr;
        const boost::intrusive_ptr<ExpressionContext> expCtx(
            new ExpressionContext(&_opCtx, collator));
        StatusWithMatchExpression statusWithMatcher =
            MatchExpressionParser::parse(filterObj, expCtx);
        verify(statusWithMatcher.isOK());
        unique_ptr<MatchExpression> filterExpr = std::move(statusWithMatcher.getValue());

        // Make a scan and have the runner own it.
        unique_ptr<WorkingSet> ws = std::make_unique<WorkingSet>();
        unique_ptr<PlanStage> ps = std::make_unique<CollectionScan>(
            &_opCtx, collection, params, ws.get(), filterExpr.get());

        auto statusWithPlanExecutor = PlanExecutor::make(
            &_opCtx, std::move(ws), std::move(ps), collection, PlanExecutor::NO_YIELD);
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

    void getRecordIds(Collection* collection,
                      CollectionScanParams::Direction direction,
                      vector<RecordId>* out) {
        WorkingSet ws;

        CollectionScanParams params;
        params.direction = direction;
        params.tailable = false;

        unique_ptr<CollectionScan> scan(
            new CollectionScan(&_opCtx, collection, params, &ws, nullptr));
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

    static int numObj() {
        return 50;
    }

protected:
    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_txnPtr;

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
    AutoGetCollectionForReadCommand ctx(&_opCtx, nss);
    auto collection = ctx.getCollection();

    // Configure the scan.
    CollectionScanParams params;
    params.direction = CollectionScanParams::FORWARD;
    params.tailable = false;

    // Make a scan and have the runner own it.
    unique_ptr<WorkingSet> ws = std::make_unique<WorkingSet>();
    unique_ptr<PlanStage> ps =
        std::make_unique<CollectionScan>(&_opCtx, collection, params, ws.get(), nullptr);

    auto statusWithPlanExecutor = PlanExecutor::make(
        &_opCtx, std::move(ws), std::move(ps), collection, PlanExecutor::NO_YIELD);
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
    AutoGetCollectionForReadCommand ctx(&_opCtx, nss);
    auto collection = ctx.getCollection();

    CollectionScanParams params;
    params.direction = CollectionScanParams::BACKWARD;
    params.tailable = false;

    unique_ptr<WorkingSet> ws = std::make_unique<WorkingSet>();
    unique_ptr<PlanStage> ps =
        std::make_unique<CollectionScan>(&_opCtx, collection, params, ws.get(), nullptr);

    auto statusWithPlanExecutor = PlanExecutor::make(
        &_opCtx, std::move(ws), std::move(ps), collection, PlanExecutor::NO_YIELD);
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

    Collection* coll = ctx.getCollection();

    // Get the RecordIds that would be returned by an in-order scan.
    vector<RecordId> recordIds;
    getRecordIds(coll, CollectionScanParams::FORWARD, &recordIds);

    // Configure the scan.
    CollectionScanParams params;
    params.direction = CollectionScanParams::FORWARD;
    params.tailable = false;

    WorkingSet ws;
    unique_ptr<PlanStage> scan(new CollectionScan(&_opCtx, coll, params, &ws, nullptr));

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
    scan->restoreState();

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
    Collection* coll = ctx.getCollection();

    // Get the RecordIds that would be returned by an in-order scan.
    vector<RecordId> recordIds;
    getRecordIds(coll, CollectionScanParams::BACKWARD, &recordIds);

    // Configure the scan.
    CollectionScanParams params;
    params.direction = CollectionScanParams::BACKWARD;
    params.tailable = false;

    WorkingSet ws;
    unique_ptr<PlanStage> scan(new CollectionScan(&_opCtx, coll, params, &ws, nullptr));

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
    scan->restoreState();

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

}  // namespace query_stage_collection_scan
