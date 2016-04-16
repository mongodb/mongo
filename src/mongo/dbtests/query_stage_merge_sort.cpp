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

#include "mongo/platform/basic.h"

#include "mongo/client/dbclientcursor.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/fetch.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/merge_sort.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/json.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/stdx/memory.h"

/**
 * This file tests db/exec/merge_sort.cpp
 */

namespace QueryStageMergeSortTests {

using std::set;
using std::string;
using std::unique_ptr;
using stdx::make_unique;

class QueryStageMergeSortTestBase {
public:
    QueryStageMergeSortTestBase() : _client(&_txn) {}

    virtual ~QueryStageMergeSortTestBase() {
        OldClientWriteContext ctx(&_txn, ns());
        _client.dropCollection(ns());
    }

    void addIndex(const BSONObj& obj) {
        ASSERT_OK(dbtests::createIndex(&_txn, ns(), obj));
    }

    IndexDescriptor* getIndex(const BSONObj& obj, Collection* coll) {
        return coll->getIndexCatalog()->findIndexByKeyPattern(&_txn, obj);
    }

    void insert(const BSONObj& obj) {
        _client.insert(ns(), obj);
    }

    void remove(const BSONObj& obj) {
        _client.remove(ns(), obj);
    }

    void getRecordIds(set<RecordId>* out, Collection* coll) {
        auto cursor = coll->getCursor(&_txn);
        while (auto record = cursor->next()) {
            out->insert(record->id);
        }
    }

    BSONObj objWithMinKey(int start) {
        BSONObjBuilder startKeyBob;
        startKeyBob.append("", start);
        startKeyBob.appendMinKey("");
        return startKeyBob.obj();
    }

    BSONObj objWithMaxKey(int start) {
        BSONObjBuilder endKeyBob;
        endKeyBob.append("", start);
        endKeyBob.appendMaxKey("");
        return endKeyBob.obj();
    }

    static const char* ns() {
        return "unittests.QueryStageMergeSort";
    }

protected:
    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _txn = *_txnPtr;

private:
    DBDirectClient _client;
};

// SERVER-1205:
// find($or[{a:1}, {b:1}]).sort({c:1}) with indices {a:1, c:1} and {b:1, c:1}.
class QueryStageMergeSortPrefixIndex : public QueryStageMergeSortTestBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, ns());
        Database* db = ctx.db();
        Collection* coll = db->getCollection(ns());
        if (!coll) {
            WriteUnitOfWork wuow(&_txn);
            coll = db->createCollection(&_txn, ns());
            wuow.commit();
        }

        const int N = 50;

        for (int i = 0; i < N; ++i) {
            insert(BSON("a" << 1 << "c" << i));
            insert(BSON("b" << 1 << "c" << i));
        }

        BSONObj firstIndex = BSON("a" << 1 << "c" << 1);
        BSONObj secondIndex = BSON("b" << 1 << "c" << 1);

        addIndex(firstIndex);
        addIndex(secondIndex);

        unique_ptr<WorkingSet> ws = make_unique<WorkingSet>();
        // Sort by c:1
        MergeSortStageParams msparams;
        msparams.pattern = BSON("c" << 1);
        MergeSortStage* ms = new MergeSortStage(&_txn, msparams, ws.get(), coll);

        // a:1
        IndexScanParams params;
        params.descriptor = getIndex(firstIndex, coll);
        params.bounds.isSimpleRange = true;
        params.bounds.startKey = objWithMinKey(1);
        params.bounds.endKey = objWithMaxKey(1);
        params.bounds.endKeyInclusive = true;
        params.direction = 1;
        ms->addChild(new IndexScan(&_txn, params, ws.get(), NULL));

        // b:1
        params.descriptor = getIndex(secondIndex, coll);
        ms->addChild(new IndexScan(&_txn, params, ws.get(), NULL));

        unique_ptr<FetchStage> fetchStage =
            make_unique<FetchStage>(&_txn, ws.get(), ms, nullptr, coll);
        // Must fetch if we want to easily pull out an obj.
        auto statusWithPlanExecutor = PlanExecutor::make(
            &_txn, std::move(ws), std::move(fetchStage), coll, PlanExecutor::YIELD_MANUAL);
        ASSERT_OK(statusWithPlanExecutor.getStatus());
        unique_ptr<PlanExecutor> exec = std::move(statusWithPlanExecutor.getValue());

        for (int i = 0; i < N; ++i) {
            BSONObj first, second;
            ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&first, NULL));
            first = first.getOwned();
            ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&second, NULL));
            ASSERT_EQUALS(first["c"].numberInt(), second["c"].numberInt());
            ASSERT_EQUALS(i, first["c"].numberInt());
            ASSERT((first.hasField("a") && second.hasField("b")) ||
                   (first.hasField("b") && second.hasField("a")));
        }

        // Should be done now.
        BSONObj foo;
        ASSERT_NOT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&foo, NULL));
    }
};

// Each inserted document appears in both indices but is deduped and returned properly/sorted.
class QueryStageMergeSortDups : public QueryStageMergeSortTestBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, ns());
        Database* db = ctx.db();
        Collection* coll = db->getCollection(ns());
        if (!coll) {
            WriteUnitOfWork wuow(&_txn);
            coll = db->createCollection(&_txn, ns());
            wuow.commit();
        }

        const int N = 50;

        for (int i = 0; i < N; ++i) {
            insert(BSON("a" << 1 << "b" << 1 << "c" << i));
            insert(BSON("a" << 1 << "b" << 1 << "c" << i));
        }

        BSONObj firstIndex = BSON("a" << 1 << "c" << 1);
        BSONObj secondIndex = BSON("b" << 1 << "c" << 1);

        addIndex(firstIndex);
        addIndex(secondIndex);

        unique_ptr<WorkingSet> ws = make_unique<WorkingSet>();
        // Sort by c:1
        MergeSortStageParams msparams;
        msparams.pattern = BSON("c" << 1);
        MergeSortStage* ms = new MergeSortStage(&_txn, msparams, ws.get(), coll);

        // a:1
        IndexScanParams params;
        params.descriptor = getIndex(firstIndex, coll);
        params.bounds.isSimpleRange = true;
        params.bounds.startKey = objWithMinKey(1);
        params.bounds.endKey = objWithMaxKey(1);
        params.bounds.endKeyInclusive = true;
        params.direction = 1;
        ms->addChild(new IndexScan(&_txn, params, ws.get(), NULL));

        // b:1
        params.descriptor = getIndex(secondIndex, coll);
        ms->addChild(new IndexScan(&_txn, params, ws.get(), NULL));
        unique_ptr<FetchStage> fetchStage =
            make_unique<FetchStage>(&_txn, ws.get(), ms, nullptr, coll);

        auto statusWithPlanExecutor = PlanExecutor::make(
            &_txn, std::move(ws), std::move(fetchStage), coll, PlanExecutor::YIELD_MANUAL);
        ASSERT_OK(statusWithPlanExecutor.getStatus());
        unique_ptr<PlanExecutor> exec = std::move(statusWithPlanExecutor.getValue());

        for (int i = 0; i < N; ++i) {
            BSONObj first, second;
            ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&first, NULL));
            first = first.getOwned();
            ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&second, NULL));
            ASSERT_EQUALS(first["c"].numberInt(), second["c"].numberInt());
            ASSERT_EQUALS(i, first["c"].numberInt());
            ASSERT((first.hasField("a") && second.hasField("b")) ||
                   (first.hasField("b") && second.hasField("a")));
        }

        // Should be done now.
        BSONObj foo;
        ASSERT_EQUALS(PlanExecutor::IS_EOF, exec->getNext(&foo, NULL));
    }
};

// Each inserted document appears in both indices, no deduping, get each result twice.
class QueryStageMergeSortDupsNoDedup : public QueryStageMergeSortTestBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, ns());
        Database* db = ctx.db();
        Collection* coll = db->getCollection(ns());
        if (!coll) {
            WriteUnitOfWork wuow(&_txn);
            coll = db->createCollection(&_txn, ns());
            wuow.commit();
        }

        const int N = 50;

        for (int i = 0; i < N; ++i) {
            insert(BSON("a" << 1 << "b" << 1 << "c" << i));
        }

        BSONObj firstIndex = BSON("a" << 1 << "c" << 1);
        BSONObj secondIndex = BSON("b" << 1 << "c" << 1);

        addIndex(firstIndex);
        addIndex(secondIndex);

        unique_ptr<WorkingSet> ws = make_unique<WorkingSet>();
        // Sort by c:1
        MergeSortStageParams msparams;
        msparams.dedup = false;
        msparams.pattern = BSON("c" << 1);
        MergeSortStage* ms = new MergeSortStage(&_txn, msparams, ws.get(), coll);

        // a:1
        IndexScanParams params;
        params.descriptor = getIndex(firstIndex, coll);
        params.bounds.isSimpleRange = true;
        params.bounds.startKey = objWithMinKey(1);
        params.bounds.endKey = objWithMaxKey(1);
        params.bounds.endKeyInclusive = true;
        params.direction = 1;
        ms->addChild(new IndexScan(&_txn, params, ws.get(), NULL));

        // b:1
        params.descriptor = getIndex(secondIndex, coll);
        ms->addChild(new IndexScan(&_txn, params, ws.get(), NULL));
        unique_ptr<FetchStage> fetchStage =
            make_unique<FetchStage>(&_txn, ws.get(), ms, nullptr, coll);

        auto statusWithPlanExecutor = PlanExecutor::make(
            &_txn, std::move(ws), std::move(fetchStage), coll, PlanExecutor::YIELD_MANUAL);
        ASSERT_OK(statusWithPlanExecutor.getStatus());
        unique_ptr<PlanExecutor> exec = std::move(statusWithPlanExecutor.getValue());

        for (int i = 0; i < N; ++i) {
            BSONObj first, second;
            // We inserted N objects but we get 2 * N from the runner because of dups.
            ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&first, NULL));
            first = first.getOwned();
            ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&second, NULL));
            ASSERT_EQUALS(first["c"].numberInt(), second["c"].numberInt());
            ASSERT_EQUALS(i, first["c"].numberInt());
            ASSERT((first.hasField("a") && second.hasField("b")) ||
                   (first.hasField("b") && second.hasField("a")));
        }

        // Should be done now.
        BSONObj foo;
        ASSERT_EQUALS(PlanExecutor::IS_EOF, exec->getNext(&foo, NULL));
    }
};

// Decreasing indices merged ok.  Basically the test above but decreasing.
class QueryStageMergeSortPrefixIndexReverse : public QueryStageMergeSortTestBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, ns());
        Database* db = ctx.db();
        Collection* coll = db->getCollection(ns());
        if (!coll) {
            WriteUnitOfWork wuow(&_txn);
            coll = db->createCollection(&_txn, ns());
            wuow.commit();
        }

        const int N = 50;

        for (int i = 0; i < N; ++i) {
            // We insert a:1 c:i for i=0..49 but in reverse order for the heck of it.
            insert(BSON("a" << 1 << "c" << N - i - 1));
            insert(BSON("b" << 1 << "c" << i));
        }

        BSONObj firstIndex = BSON("a" << 1 << "c" << -1);
        BSONObj secondIndex = BSON("b" << 1 << "c" << -1);

        addIndex(firstIndex);
        addIndex(secondIndex);

        unique_ptr<WorkingSet> ws = make_unique<WorkingSet>();
        // Sort by c:-1
        MergeSortStageParams msparams;
        msparams.pattern = BSON("c" << -1);
        MergeSortStage* ms = new MergeSortStage(&_txn, msparams, ws.get(), coll);

        // a:1
        IndexScanParams params;
        params.descriptor = getIndex(firstIndex, coll);
        params.bounds.isSimpleRange = true;
        params.bounds.startKey = objWithMaxKey(1);
        params.bounds.endKey = objWithMinKey(1);
        params.bounds.endKeyInclusive = true;
        // This is the direction along the index.
        params.direction = 1;
        ms->addChild(new IndexScan(&_txn, params, ws.get(), NULL));

        // b:1
        params.descriptor = getIndex(secondIndex, coll);
        ms->addChild(new IndexScan(&_txn, params, ws.get(), NULL));
        unique_ptr<FetchStage> fetchStage =
            make_unique<FetchStage>(&_txn, ws.get(), ms, nullptr, coll);

        auto statusWithPlanExecutor = PlanExecutor::make(
            &_txn, std::move(ws), std::move(fetchStage), coll, PlanExecutor::YIELD_MANUAL);
        ASSERT_OK(statusWithPlanExecutor.getStatus());
        unique_ptr<PlanExecutor> exec = std::move(statusWithPlanExecutor.getValue());

        for (int i = 0; i < N; ++i) {
            BSONObj first, second;
            ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&first, NULL));
            first = first.getOwned();
            ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&second, NULL));
            ASSERT_EQUALS(first["c"].numberInt(), second["c"].numberInt());
            ASSERT_EQUALS(N - i - 1, first["c"].numberInt());
            ASSERT((first.hasField("a") && second.hasField("b")) ||
                   (first.hasField("b") && second.hasField("a")));
        }

        // Should be done now.
        BSONObj foo;
        ASSERT_EQUALS(PlanExecutor::IS_EOF, exec->getNext(&foo, NULL));
    }
};

// One stage EOF immediately
class QueryStageMergeSortOneStageEOF : public QueryStageMergeSortTestBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, ns());
        Database* db = ctx.db();
        Collection* coll = db->getCollection(ns());
        if (!coll) {
            WriteUnitOfWork wuow(&_txn);
            coll = db->createCollection(&_txn, ns());
            wuow.commit();
        }

        const int N = 50;

        for (int i = 0; i < N; ++i) {
            insert(BSON("a" << 1 << "c" << i));
            insert(BSON("b" << 1 << "c" << i));
        }

        BSONObj firstIndex = BSON("a" << 1 << "c" << 1);
        BSONObj secondIndex = BSON("b" << 1 << "c" << 1);

        addIndex(firstIndex);
        addIndex(secondIndex);

        unique_ptr<WorkingSet> ws = make_unique<WorkingSet>();
        // Sort by c:1
        MergeSortStageParams msparams;
        msparams.pattern = BSON("c" << 1);
        MergeSortStage* ms = new MergeSortStage(&_txn, msparams, ws.get(), coll);

        // a:1
        IndexScanParams params;
        params.descriptor = getIndex(firstIndex, coll);
        params.bounds.isSimpleRange = true;
        params.bounds.startKey = objWithMinKey(1);
        params.bounds.endKey = objWithMaxKey(1);
        params.bounds.endKeyInclusive = true;
        params.direction = 1;
        ms->addChild(new IndexScan(&_txn, params, ws.get(), NULL));

        // b:51 (EOF)
        params.descriptor = getIndex(secondIndex, coll);
        params.bounds.startKey = BSON("" << 51 << "" << MinKey);
        params.bounds.endKey = BSON("" << 51 << "" << MaxKey);
        ms->addChild(new IndexScan(&_txn, params, ws.get(), NULL));
        unique_ptr<FetchStage> fetchStage =
            make_unique<FetchStage>(&_txn, ws.get(), ms, nullptr, coll);

        auto statusWithPlanExecutor = PlanExecutor::make(
            &_txn, std::move(ws), std::move(fetchStage), coll, PlanExecutor::YIELD_MANUAL);
        ASSERT_OK(statusWithPlanExecutor.getStatus());
        unique_ptr<PlanExecutor> exec = std::move(statusWithPlanExecutor.getValue());

        // Only getting results from the a:1 index scan.
        for (int i = 0; i < N; ++i) {
            BSONObj obj;
            ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, NULL));
            ASSERT_EQUALS(i, obj["c"].numberInt());
            ASSERT_EQUALS(1, obj["a"].numberInt());
        }

        // Should be done now.
        BSONObj foo;
        ASSERT_EQUALS(PlanExecutor::IS_EOF, exec->getNext(&foo, NULL));
    }
};

// N stages each have 1 result
class QueryStageMergeSortManyShort : public QueryStageMergeSortTestBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, ns());
        Database* db = ctx.db();
        Collection* coll = db->getCollection(ns());
        if (!coll) {
            WriteUnitOfWork wuow(&_txn);
            coll = db->createCollection(&_txn, ns());
            wuow.commit();
        }

        unique_ptr<WorkingSet> ws = make_unique<WorkingSet>();
        // Sort by foo:1
        MergeSortStageParams msparams;
        msparams.pattern = BSON("foo" << 1);
        MergeSortStage* ms = new MergeSortStage(&_txn, msparams, ws.get(), coll);

        IndexScanParams params;
        params.bounds.isSimpleRange = true;
        params.bounds.startKey = objWithMinKey(1);
        params.bounds.endKey = objWithMaxKey(1);
        params.bounds.endKeyInclusive = true;
        params.direction = 1;

        int numIndices = 20;
        for (int i = 0; i < numIndices; ++i) {
            // 'a', 'b', ...
            string index(1, 'a' + i);
            insert(BSON(index << 1 << "foo" << i));

            BSONObj indexSpec = BSON(index << 1 << "foo" << 1);
            addIndex(indexSpec);
            params.descriptor = getIndex(indexSpec, coll);
            ms->addChild(new IndexScan(&_txn, params, ws.get(), NULL));
        }
        unique_ptr<FetchStage> fetchStage =
            make_unique<FetchStage>(&_txn, ws.get(), ms, nullptr, coll);

        auto statusWithPlanExecutor = PlanExecutor::make(
            &_txn, std::move(ws), std::move(fetchStage), coll, PlanExecutor::YIELD_MANUAL);
        ASSERT_OK(statusWithPlanExecutor.getStatus());
        unique_ptr<PlanExecutor> exec = std::move(statusWithPlanExecutor.getValue());

        for (int i = 0; i < numIndices; ++i) {
            BSONObj obj;
            ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, NULL));
            ASSERT_EQUALS(i, obj["foo"].numberInt());
            string index(1, 'a' + i);
            ASSERT_EQUALS(1, obj[index].numberInt());
        }

        // Should be done now.
        BSONObj foo;
        ASSERT_EQUALS(PlanExecutor::IS_EOF, exec->getNext(&foo, NULL));
    }
};

// Invalidation mid-run
class QueryStageMergeSortInvalidation : public QueryStageMergeSortTestBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, ns());
        Database* db = ctx.db();
        Collection* coll = db->getCollection(ns());
        if (!coll) {
            WriteUnitOfWork wuow(&_txn);
            coll = db->createCollection(&_txn, ns());
            wuow.commit();
        }

        WorkingSet ws;
        // Sort by foo:1
        MergeSortStageParams msparams;
        msparams.pattern = BSON("foo" << 1);
        auto ms = make_unique<MergeSortStage>(&_txn, msparams, &ws, coll);

        IndexScanParams params;
        params.bounds.isSimpleRange = true;
        params.bounds.startKey = objWithMinKey(1);
        params.bounds.endKey = objWithMaxKey(1);
        params.bounds.endKeyInclusive = true;
        params.direction = 1;

        // Index 'a'+i has foo equal to 'i'.

        int numIndices = 20;
        for (int i = 0; i < numIndices; ++i) {
            // 'a', 'b', ...
            string index(1, 'a' + i);
            insert(BSON(index << 1 << "foo" << i));

            BSONObj indexSpec = BSON(index << 1 << "foo" << 1);
            addIndex(indexSpec);
            params.descriptor = getIndex(indexSpec, coll);
            ms->addChild(new IndexScan(&_txn, params, &ws, NULL));
        }

        set<RecordId> recordIds;
        getRecordIds(&recordIds, coll);

        set<RecordId>::iterator it = recordIds.begin();

        // Get 10 results.  Should be getting results in order of 'recordIds'.
        int count = 0;
        while (!ms->isEOF() && count < 10) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState status = ms->work(&id);
            if (PlanStage::ADVANCED != status) {
                continue;
            }

            WorkingSetMember* member = ws.get(id);
            ASSERT_EQUALS(member->recordId, *it);
            BSONElement elt;
            string index(1, 'a' + count);
            ASSERT(member->getFieldDotted(index, &elt));
            ASSERT_EQUALS(1, elt.numberInt());
            ASSERT(member->getFieldDotted("foo", &elt));
            ASSERT_EQUALS(count, elt.numberInt());
            ++count;
            ++it;
        }

        // Invalidate recordIds[11].  Should force a fetch and return the deleted document.
        ms->saveState();
        ms->invalidate(&_txn, *it, INVALIDATION_DELETION);
        ms->restoreState();

        // Make sure recordIds[11] was fetched for us.
        {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState status;
            do {
                status = ms->work(&id);
            } while (PlanStage::ADVANCED != status);

            WorkingSetMember* member = ws.get(id);
            ASSERT(!member->hasRecordId());
            ASSERT(member->hasObj());
            string index(1, 'a' + count);
            BSONElement elt;
            ASSERT_TRUE(member->getFieldDotted(index, &elt));
            ASSERT_EQUALS(1, elt.numberInt());
            ASSERT(member->getFieldDotted("foo", &elt));
            ASSERT_EQUALS(count, elt.numberInt());

            ++it;
            ++count;
        }

        // And get the rest.
        while (!ms->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState status = ms->work(&id);
            if (PlanStage::ADVANCED != status) {
                continue;
            }

            WorkingSetMember* member = ws.get(id);
            ASSERT_EQUALS(member->recordId, *it);
            BSONElement elt;
            string index(1, 'a' + count);
            ASSERT_TRUE(member->getFieldDotted(index, &elt));
            ASSERT_EQUALS(1, elt.numberInt());
            ASSERT(member->getFieldDotted("foo", &elt));
            ASSERT_EQUALS(count, elt.numberInt());
            ++count;
            ++it;
        }
    }
};

// Test that if a WSM buffered inside the merge sort stage gets updated, we return the document and
// then correctly dedup if we see the same RecordId again.
class QueryStageMergeSortInvalidationMutationDedup : public QueryStageMergeSortTestBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, ns());
        Database* db = ctx.db();
        Collection* coll = db->getCollection(ns());
        if (!coll) {
            WriteUnitOfWork wuow(&_txn);
            coll = db->createCollection(&_txn, ns());
            wuow.commit();
        }

        // Insert data.
        insert(BSON("_id" << 4 << "a" << 4));
        insert(BSON("_id" << 5 << "a" << 5));
        insert(BSON("_id" << 6 << "a" << 6));

        addIndex(BSON("a" << 1));

        std::set<RecordId> rids;
        getRecordIds(&rids, coll);
        set<RecordId>::iterator it = rids.begin();

        WorkingSet ws;
        WorkingSetMember* member;
        MergeSortStageParams msparams;
        msparams.pattern = BSON("a" << 1);
        auto ms = stdx::make_unique<MergeSortStage>(&_txn, msparams, &ws, coll);

        // First child scans [5, 10].
        {
            IndexScanParams params;
            params.descriptor = getIndex(BSON("a" << 1), coll);
            params.bounds.isSimpleRange = true;
            params.bounds.startKey = BSON("" << 5);
            params.bounds.endKey = BSON("" << 10);
            params.bounds.endKeyInclusive = true;
            params.direction = 1;
            auto fetchStage = stdx::make_unique<FetchStage>(
                &_txn, &ws, new IndexScan(&_txn, params, &ws, nullptr), nullptr, coll);
            ms->addChild(fetchStage.release());
        }

        // Second child scans [4, 10].
        {
            IndexScanParams params;
            params.descriptor = getIndex(BSON("a" << 1), coll);
            params.bounds.isSimpleRange = true;
            params.bounds.startKey = BSON("" << 4);
            params.bounds.endKey = BSON("" << 10);
            params.bounds.endKeyInclusive = true;
            params.direction = 1;
            auto fetchStage = stdx::make_unique<FetchStage>(
                &_txn, &ws, new IndexScan(&_txn, params, &ws, nullptr), nullptr, coll);
            ms->addChild(fetchStage.release());
        }

        // First doc should be {a: 4}.
        member = getNextResult(&ws, ms.get());
        ASSERT_EQ(member->getState(), WorkingSetMember::RID_AND_OBJ);
        ASSERT_EQ(member->recordId, *it);
        ASSERT_EQ(member->obj.value(), BSON("_id" << 4 << "a" << 4));
        ++it;

        // Doc {a: 5} gets invalidated by an update.
        ms->invalidate(&_txn, *it, INVALIDATION_MUTATION);

        // Invalidated doc {a: 5} should still get returned.
        member = getNextResult(&ws, ms.get());
        ASSERT_EQ(member->getState(), WorkingSetMember::OWNED_OBJ);
        ASSERT_EQ(member->obj.value(), BSON("_id" << 5 << "a" << 5));
        ++it;

        // We correctly dedup the invalidated doc and return {a: 6} next.
        member = getNextResult(&ws, ms.get());
        ASSERT_EQ(member->getState(), WorkingSetMember::RID_AND_OBJ);
        ASSERT_EQ(member->recordId, *it);
        ASSERT_EQ(member->obj.value(), BSON("_id" << 6 << "a" << 6));
    }

private:
    WorkingSetMember* getNextResult(WorkingSet* ws, PlanStage* stage) {
        while (!stage->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState status = stage->work(&id);
            if (PlanStage::ADVANCED != status) {
                continue;
            }

            return ws->get(id);
        }

        FAIL("Expected to produce another result but hit EOF");
        return nullptr;
    }
};

class QueryStageMergeSortStringsWithNullCollation : public QueryStageMergeSortTestBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, ns());
        Database* db = ctx.db();
        Collection* coll = db->getCollection(ns());
        if (!coll) {
            WriteUnitOfWork wuow(&_txn);
            coll = db->createCollection(&_txn, ns());
            wuow.commit();
        }

        const int N = 50;

        for (int i = 0; i < N; ++i) {
            insert(BSON("a" << 1 << "c" << i << "d"
                            << "abc"));
            insert(BSON("b" << 1 << "c" << i << "d"
                            << "cba"));
        }

        BSONObj firstIndex = BSON("a" << 1 << "c" << 1 << "d" << 1);
        BSONObj secondIndex = BSON("b" << 1 << "c" << 1 << "d" << 1);

        addIndex(firstIndex);
        addIndex(secondIndex);

        unique_ptr<WorkingSet> ws = make_unique<WorkingSet>();
        // Sort by c:1, d:1
        MergeSortStageParams msparams;
        msparams.pattern = BSON("c" << 1 << "d" << 1);
        msparams.collator = nullptr;
        MergeSortStage* ms = new MergeSortStage(&_txn, msparams, ws.get(), coll);

        // a:1
        IndexScanParams params;
        params.descriptor = getIndex(firstIndex, coll);
        params.bounds.isSimpleRange = true;
        params.bounds.startKey = objWithMinKey(1);
        params.bounds.endKey = objWithMaxKey(1);
        params.bounds.endKeyInclusive = true;
        params.direction = 1;
        ms->addChild(new IndexScan(&_txn, params, ws.get(), NULL));

        // b:1
        params.descriptor = getIndex(secondIndex, coll);
        ms->addChild(new IndexScan(&_txn, params, ws.get(), NULL));

        unique_ptr<FetchStage> fetchStage =
            make_unique<FetchStage>(&_txn, ws.get(), ms, nullptr, coll);
        // Must fetch if we want to easily pull out an obj.
        auto statusWithPlanExecutor = PlanExecutor::make(
            &_txn, std::move(ws), std::move(fetchStage), coll, PlanExecutor::YIELD_MANUAL);
        ASSERT_OK(statusWithPlanExecutor.getStatus());
        unique_ptr<PlanExecutor> exec = std::move(statusWithPlanExecutor.getValue());

        for (int i = 0; i < N; ++i) {
            BSONObj first, second;
            ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&first, NULL));
            first = first.getOwned();
            ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&second, NULL));
            ASSERT_EQUALS(first["c"].numberInt(), second["c"].numberInt());
            ASSERT_EQUALS(i, first["c"].numberInt());
            // {a: 1, c: i, d: "abc"} should precede {b: 1, c: i, d: "bca"}.
            ASSERT(first.hasField("a") && second.hasField("b"));
        }

        // Should be done now.
        BSONObj foo;
        ASSERT_NOT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&foo, NULL));
    }
};

class QueryStageMergeSortStringsRespectsCollation : public QueryStageMergeSortTestBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, ns());
        Database* db = ctx.db();
        Collection* coll = db->getCollection(ns());
        if (!coll) {
            WriteUnitOfWork wuow(&_txn);
            coll = db->createCollection(&_txn, ns());
            wuow.commit();
        }

        const int N = 50;

        for (int i = 0; i < N; ++i) {
            insert(BSON("a" << 1 << "c" << i << "d"
                            << "abc"));
            insert(BSON("b" << 1 << "c" << i << "d"
                            << "cba"));
        }

        BSONObj firstIndex = BSON("a" << 1 << "c" << 1 << "d" << 1);
        BSONObj secondIndex = BSON("b" << 1 << "c" << 1 << "d" << 1);

        addIndex(firstIndex);
        addIndex(secondIndex);

        unique_ptr<WorkingSet> ws = make_unique<WorkingSet>();
        // Sort by c:1, d:1
        MergeSortStageParams msparams;
        msparams.pattern = BSON("c" << 1 << "d" << 1);
        CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
        msparams.collator = &collator;
        MergeSortStage* ms = new MergeSortStage(&_txn, msparams, ws.get(), coll);

        // a:1
        IndexScanParams params;
        params.descriptor = getIndex(firstIndex, coll);
        params.bounds.isSimpleRange = true;
        params.bounds.startKey = objWithMinKey(1);
        params.bounds.endKey = objWithMaxKey(1);
        params.bounds.endKeyInclusive = true;
        params.direction = 1;
        ms->addChild(new IndexScan(&_txn, params, ws.get(), NULL));

        // b:1
        params.descriptor = getIndex(secondIndex, coll);
        ms->addChild(new IndexScan(&_txn, params, ws.get(), NULL));

        unique_ptr<FetchStage> fetchStage =
            make_unique<FetchStage>(&_txn, ws.get(), ms, nullptr, coll);
        // Must fetch if we want to easily pull out an obj.
        auto statusWithPlanExecutor = PlanExecutor::make(
            &_txn, std::move(ws), std::move(fetchStage), coll, PlanExecutor::YIELD_MANUAL);
        ASSERT_OK(statusWithPlanExecutor.getStatus());
        unique_ptr<PlanExecutor> exec = std::move(statusWithPlanExecutor.getValue());

        for (int i = 0; i < N; ++i) {
            BSONObj first, second;
            ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&first, NULL));
            first = first.getOwned();
            ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&second, NULL));
            ASSERT_EQUALS(first["c"].numberInt(), second["c"].numberInt());
            ASSERT_EQUALS(i, first["c"].numberInt());
            // {b: 1, c: i, d: "cba"} should precede {a: 1, c: i, d: "abc"}.
            ASSERT(first.hasField("b") && second.hasField("a"));
        }

        // Should be done now.
        BSONObj foo;
        ASSERT_NOT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&foo, NULL));
    }
};

class All : public Suite {
public:
    All() : Suite("query_stage_merge_sort_test") {}

    void setupTests() {
        add<QueryStageMergeSortPrefixIndex>();
        add<QueryStageMergeSortDups>();
        add<QueryStageMergeSortDupsNoDedup>();
        add<QueryStageMergeSortPrefixIndexReverse>();
        add<QueryStageMergeSortOneStageEOF>();
        add<QueryStageMergeSortManyShort>();
        add<QueryStageMergeSortInvalidation>();
        add<QueryStageMergeSortInvalidationMutationDedup>();
        add<QueryStageMergeSortStringsWithNullCollation>();
        add<QueryStageMergeSortStringsRespectsCollation>();
    }
};

SuiteInstance<All> queryStageMergeSortTest;

}  // namespace
