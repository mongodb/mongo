/**
 *    Copyright (C) 2013-2014 MongoDB Inc.
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

/**
 * This file tests db/exec/collection_scan.cpp.
 */

#include "mongo/platform/basic.h"

#include "mongo/client/dbclientcursor.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/fail_point_service.h"

namespace QueryStageCollectionScan {

using std::unique_ptr;
using std::vector;
using stdx::make_unique;

//
// Stage-specific tests.
//

class QueryStageCollectionScanBase {
public:
    QueryStageCollectionScanBase() : _client(&_txn) {
        OldClientWriteContext ctx(&_txn, ns());

        for (int i = 0; i < numObj(); ++i) {
            BSONObjBuilder bob;
            bob.append("foo", i);
            _client.insert(ns(), bob.obj());
        }
    }

    virtual ~QueryStageCollectionScanBase() {
        OldClientWriteContext ctx(&_txn, ns());
        _client.dropCollection(ns());
    }

    void remove(const BSONObj& obj) {
        _client.remove(ns(), obj);
    }

    int countResults(CollectionScanParams::Direction direction, const BSONObj& filterObj) {
        AutoGetCollectionForRead ctx(&_txn, ns());

        // Configure the scan.
        CollectionScanParams params;
        params.collection = ctx.getCollection();
        params.direction = direction;
        params.tailable = false;

        // Make the filter.
        const CollatorInterface* collator = nullptr;
        StatusWithMatchExpression statusWithMatcher = MatchExpressionParser::parse(
            filterObj, ExtensionsCallbackDisallowExtensions(), collator);
        verify(statusWithMatcher.isOK());
        unique_ptr<MatchExpression> filterExpr = std::move(statusWithMatcher.getValue());

        // Make a scan and have the runner own it.
        unique_ptr<WorkingSet> ws = make_unique<WorkingSet>();
        unique_ptr<PlanStage> ps =
            make_unique<CollectionScan>(&_txn, params, ws.get(), filterExpr.get());

        auto statusWithPlanExecutor = PlanExecutor::make(
            &_txn, std::move(ws), std::move(ps), params.collection, PlanExecutor::YIELD_MANUAL);
        ASSERT_OK(statusWithPlanExecutor.getStatus());
        unique_ptr<PlanExecutor> exec = std::move(statusWithPlanExecutor.getValue());

        // Use the runner to count the number of objects scanned.
        int count = 0;
        PlanExecutor::ExecState state;
        for (BSONObj obj; PlanExecutor::ADVANCED == (state = exec->getNext(&obj, NULL));) {
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
        params.collection = collection;
        params.direction = direction;
        params.tailable = false;

        unique_ptr<CollectionScan> scan(new CollectionScan(&_txn, params, &ws, NULL));
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

    static const char* ns() {
        return "unittests.QueryStageCollectionScan";
    }

protected:
    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _txn = *_txnPtr;

private:
    DBDirectClient _client;
};


//
// Go forwards, get everything.
//
class QueryStageCollscanBasicForward : public QueryStageCollectionScanBase {
public:
    void run() {
        ASSERT_EQUALS(numObj(), countResults(CollectionScanParams::FORWARD, BSONObj()));
    }
};

//
// Go backwards, get everything.
//

class QueryStageCollscanBasicBackward : public QueryStageCollectionScanBase {
public:
    void run() {
        ASSERT_EQUALS(numObj(), countResults(CollectionScanParams::BACKWARD, BSONObj()));
    }
};

//
// Go forwards and match half the docs.
//

class QueryStageCollscanBasicForwardWithMatch : public QueryStageCollectionScanBase {
public:
    void run() {
        BSONObj obj = BSON("foo" << BSON("$lt" << 25));
        ASSERT_EQUALS(25, countResults(CollectionScanParams::FORWARD, obj));
    }
};

//
// Go backwards and match half the docs.
//

class QueryStageCollscanBasicBackwardWithMatch : public QueryStageCollectionScanBase {
public:
    void run() {
        BSONObj obj = BSON("foo" << BSON("$lt" << 25));
        ASSERT_EQUALS(25, countResults(CollectionScanParams::BACKWARD, obj));
    }
};

//
// Get objects in the order we inserted them.
//

class QueryStageCollscanObjectsInOrderForward : public QueryStageCollectionScanBase {
public:
    void run() {
        AutoGetCollectionForRead ctx(&_txn, ns());

        // Configure the scan.
        CollectionScanParams params;
        params.collection = ctx.getCollection();
        params.direction = CollectionScanParams::FORWARD;
        params.tailable = false;

        // Make a scan and have the runner own it.
        unique_ptr<WorkingSet> ws = make_unique<WorkingSet>();
        unique_ptr<PlanStage> ps = make_unique<CollectionScan>(&_txn, params, ws.get(), nullptr);

        auto statusWithPlanExecutor = PlanExecutor::make(
            &_txn, std::move(ws), std::move(ps), params.collection, PlanExecutor::YIELD_MANUAL);
        ASSERT_OK(statusWithPlanExecutor.getStatus());
        unique_ptr<PlanExecutor> exec = std::move(statusWithPlanExecutor.getValue());

        int count = 0;
        PlanExecutor::ExecState state;
        for (BSONObj obj; PlanExecutor::ADVANCED == (state = exec->getNext(&obj, NULL));) {
            // Make sure we get the objects in the order we want
            ASSERT_EQUALS(count, obj["foo"].numberInt());
            ++count;
        }
        ASSERT_EQUALS(PlanExecutor::IS_EOF, state);
        ASSERT_EQUALS(numObj(), count);
    }
};

//
// Get objects in the reverse order we inserted them when we go backwards.
//

class QueryStageCollscanObjectsInOrderBackward : public QueryStageCollectionScanBase {
public:
    void run() {
        AutoGetCollectionForRead ctx(&_txn, ns());

        CollectionScanParams params;
        params.collection = ctx.getCollection();
        params.direction = CollectionScanParams::BACKWARD;
        params.tailable = false;

        unique_ptr<WorkingSet> ws = make_unique<WorkingSet>();
        unique_ptr<PlanStage> ps = make_unique<CollectionScan>(&_txn, params, ws.get(), nullptr);

        auto statusWithPlanExecutor = PlanExecutor::make(
            &_txn, std::move(ws), std::move(ps), params.collection, PlanExecutor::YIELD_MANUAL);
        ASSERT_OK(statusWithPlanExecutor.getStatus());
        unique_ptr<PlanExecutor> exec = std::move(statusWithPlanExecutor.getValue());

        int count = 0;
        PlanExecutor::ExecState state;
        for (BSONObj obj; PlanExecutor::ADVANCED == (state = exec->getNext(&obj, NULL));) {
            ++count;
            ASSERT_EQUALS(numObj() - count, obj["foo"].numberInt());
        }
        ASSERT_EQUALS(PlanExecutor::IS_EOF, state);
        ASSERT_EQUALS(numObj(), count);
    }
};

//
// Scan through half the objects, delete the one we're about to fetch, then expect to get the
// "next" object we would have gotten after that.
//

class QueryStageCollscanInvalidateUpcomingObject : public QueryStageCollectionScanBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, ns());

        Collection* coll = ctx.getCollection();

        // Get the RecordIds that would be returned by an in-order scan.
        vector<RecordId> recordIds;
        getRecordIds(coll, CollectionScanParams::FORWARD, &recordIds);

        // Configure the scan.
        CollectionScanParams params;
        params.collection = coll;
        params.direction = CollectionScanParams::FORWARD;
        params.tailable = false;

        WorkingSet ws;
        unique_ptr<CollectionScan> scan(new CollectionScan(&_txn, params, &ws, NULL));

        int count = 0;
        while (count < 10) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState state = scan->work(&id);
            if (PlanStage::ADVANCED == state) {
                WorkingSetMember* member = ws.get(id);
                ASSERT_EQUALS(coll->docFor(&_txn, recordIds[count]).value()["foo"].numberInt(),
                              member->obj.value()["foo"].numberInt());
                ++count;
            }
        }

        // Remove recordIds[count].
        scan->saveState();
        {
            WriteUnitOfWork wunit(&_txn);
            scan->invalidate(&_txn, recordIds[count], INVALIDATION_DELETION);
            wunit.commit();  // to avoid rollback of the invalidate
        }
        remove(coll->docFor(&_txn, recordIds[count]).value());
        scan->restoreState();

        // Skip over recordIds[count].
        ++count;

        // Expect the rest.
        while (!scan->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState state = scan->work(&id);
            if (PlanStage::ADVANCED == state) {
                WorkingSetMember* member = ws.get(id);
                ASSERT_EQUALS(coll->docFor(&_txn, recordIds[count]).value()["foo"].numberInt(),
                              member->obj.value()["foo"].numberInt());
                ++count;
            }
        }

        ASSERT_EQUALS(numObj(), count);
    }
};

//
// Scan through half the objects, delete the one we're about to fetch, then expect to get the
// "next" object we would have gotten after that.  But, do it in reverse!
//

class QueryStageCollscanInvalidateUpcomingObjectBackward : public QueryStageCollectionScanBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, ns());
        Collection* coll = ctx.getCollection();

        // Get the RecordIds that would be returned by an in-order scan.
        vector<RecordId> recordIds;
        getRecordIds(coll, CollectionScanParams::BACKWARD, &recordIds);

        // Configure the scan.
        CollectionScanParams params;
        params.collection = coll;
        params.direction = CollectionScanParams::BACKWARD;
        params.tailable = false;

        WorkingSet ws;
        unique_ptr<CollectionScan> scan(new CollectionScan(&_txn, params, &ws, NULL));

        int count = 0;
        while (count < 10) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState state = scan->work(&id);
            if (PlanStage::ADVANCED == state) {
                WorkingSetMember* member = ws.get(id);
                ASSERT_EQUALS(coll->docFor(&_txn, recordIds[count]).value()["foo"].numberInt(),
                              member->obj.value()["foo"].numberInt());
                ++count;
            }
        }

        // Remove recordIds[count].
        scan->saveState();
        {
            WriteUnitOfWork wunit(&_txn);
            scan->invalidate(&_txn, recordIds[count], INVALIDATION_DELETION);
            wunit.commit();  // to avoid rollback of the invalidate
        }
        remove(coll->docFor(&_txn, recordIds[count]).value());
        scan->restoreState();

        // Skip over recordIds[count].
        ++count;

        // Expect the rest.
        while (!scan->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState state = scan->work(&id);
            if (PlanStage::ADVANCED == state) {
                WorkingSetMember* member = ws.get(id);
                ASSERT_EQUALS(coll->docFor(&_txn, recordIds[count]).value()["foo"].numberInt(),
                              member->obj.value()["foo"].numberInt());
                ++count;
            }
        }

        ASSERT_EQUALS(numObj(), count);
    }
};

class All : public Suite {
public:
    All() : Suite("QueryStageCollectionScan") {}

    void setupTests() {
        // Stage-specific tests below.
        add<QueryStageCollscanBasicForward>();
        add<QueryStageCollscanBasicBackward>();
        add<QueryStageCollscanBasicForwardWithMatch>();
        add<QueryStageCollscanBasicBackwardWithMatch>();
        add<QueryStageCollscanObjectsInOrderForward>();
        add<QueryStageCollscanObjectsInOrderBackward>();
        add<QueryStageCollscanInvalidateUpcomingObject>();
        add<QueryStageCollscanInvalidateUpcomingObjectBackward>();
    }
};

SuiteInstance<All> all;
}
