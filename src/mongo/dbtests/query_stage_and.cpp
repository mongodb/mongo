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
 * This file tests db/exec/and_*.cpp and RecordId invalidation.  RecordId invalidation forces a
 * fetch so we cannot test it outside of a dbtest.
 */


#include "mongo/platform/basic.h"

#include "mongo/client/dbclientcursor.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/and_hash.h"
#include "mongo/db/exec/and_sorted.h"
#include "mongo/db/exec/fetch.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/queued_data_stage.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/mongoutils/str.h"

namespace QueryStageAnd {

using std::set;
using std::shared_ptr;
using std::unique_ptr;
using stdx::make_unique;

class QueryStageAndBase {
public:
    QueryStageAndBase() : _client(&_txn) {}

    virtual ~QueryStageAndBase() {
        _client.dropCollection(ns());
    }

    void addIndex(const BSONObj& obj) {
        ASSERT_OK(dbtests::createIndex(&_txn, ns(), obj));
    }

    IndexDescriptor* getIndex(const BSONObj& obj, Collection* coll) {
        IndexDescriptor* descriptor = coll->getIndexCatalog()->findIndexByKeyPattern(&_txn, obj);
        if (NULL == descriptor) {
            FAIL(mongoutils::str::stream() << "Unable to find index with key pattern " << obj);
        }
        return descriptor;
    }

    void getRecordIds(set<RecordId>* out, Collection* coll) {
        auto cursor = coll->getCursor(&_txn);
        while (auto record = cursor->next()) {
            out->insert(record->id);
        }
    }

    void insert(const BSONObj& obj) {
        _client.insert(ns(), obj);
    }

    void remove(const BSONObj& obj) {
        _client.remove(ns(), obj);
    }

    /**
     * Executes plan stage until EOF.
     * Returns number of results seen if execution reaches EOF successfully.
     * Otherwise, returns -1 on stage failure.
     */
    int countResults(PlanStage* stage) {
        int count = 0;
        while (!stage->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState status = stage->work(&id);
            if (PlanStage::FAILURE == status) {
                return -1;
            }
            if (PlanStage::ADVANCED != status) {
                continue;
            }
            ++count;
        }
        return count;
    }

    /**
     * Gets the next result from 'stage'.
     *
     * Fails if the stage fails or returns DEAD, if the returned working
     * set member is not fetched, or if there are no more results.
     */
    BSONObj getNext(PlanStage* stage, WorkingSet* ws) {
        while (!stage->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState status = stage->work(&id);

            // We shouldn't fail or be dead.
            ASSERT(PlanStage::FAILURE != status);
            ASSERT(PlanStage::DEAD != status);

            if (PlanStage::ADVANCED != status) {
                continue;
            }

            WorkingSetMember* member = ws->get(id);
            ASSERT(member->hasObj());
            return member->obj.value();
        }

        // We failed to produce a result.
        ASSERT(false);
        return BSONObj();
    }

    static const char* ns() {
        return "unittests.QueryStageAnd";
    }

protected:
    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _txn = *_txnPtr;

private:
    DBDirectClient _client;
};

//
// Hash AND tests
//

/**
 * Invalidate a RecordId held by a hashed AND before the AND finishes evaluating.  The AND should
 * process all other data just fine and flag the invalidated RecordId in the WorkingSet.
 */
class QueryStageAndHashInvalidation : public QueryStageAndBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, ns());
        Database* db = ctx.db();
        Collection* coll = ctx.getCollection();
        if (!coll) {
            WriteUnitOfWork wuow(&_txn);
            coll = db->createCollection(&_txn, ns());
            wuow.commit();
        }

        for (int i = 0; i < 50; ++i) {
            insert(BSON("foo" << i << "bar" << i));
        }

        addIndex(BSON("foo" << 1));
        addIndex(BSON("bar" << 1));

        WorkingSet ws;
        auto ah = make_unique<AndHashStage>(&_txn, &ws, coll);

        // Foo <= 20
        IndexScanParams params;
        params.descriptor = getIndex(BSON("foo" << 1), coll);
        params.bounds.isSimpleRange = true;
        params.bounds.startKey = BSON("" << 20);
        params.bounds.endKey = BSONObj();
        params.bounds.endKeyInclusive = true;
        params.direction = -1;
        ah->addChild(new IndexScan(&_txn, params, &ws, NULL));

        // Bar >= 10
        params.descriptor = getIndex(BSON("bar" << 1), coll);
        params.bounds.startKey = BSON("" << 10);
        params.bounds.endKey = BSONObj();
        params.bounds.endKeyInclusive = true;
        params.direction = 1;
        ah->addChild(new IndexScan(&_txn, params, &ws, NULL));

        // ah reads the first child into its hash table.
        // ah should read foo=20, foo=19, ..., foo=0 in that order.
        // Read half of them...
        for (int i = 0; i < 10; ++i) {
            WorkingSetID out;
            PlanStage::StageState status = ah->work(&out);
            ASSERT_EQUALS(PlanStage::NEED_TIME, status);
        }

        // ...yield
        ah->saveState();
        // ...invalidate one of the read objects
        set<RecordId> data;
        getRecordIds(&data, coll);
        size_t memUsageBefore = ah->getMemUsage();
        for (set<RecordId>::const_iterator it = data.begin(); it != data.end(); ++it) {
            if (coll->docFor(&_txn, *it).value()["foo"].numberInt() == 15) {
                ah->invalidate(&_txn, *it, INVALIDATION_DELETION);
                remove(coll->docFor(&_txn, *it).value());
                break;
            }
        }
        size_t memUsageAfter = ah->getMemUsage();
        ah->restoreState();

        // Invalidating a read object should decrease memory usage.
        ASSERT_LESS_THAN(memUsageAfter, memUsageBefore);

        // And expect to find foo==15 it flagged for review.
        const unordered_set<WorkingSetID>& flagged = ws.getFlagged();
        ASSERT_EQUALS(size_t(1), flagged.size());

        // Expect to find the right value of foo in the flagged item.
        WorkingSetMember* member = ws.get(*flagged.begin());
        ASSERT_TRUE(NULL != member);
        ASSERT_EQUALS(WorkingSetMember::OWNED_OBJ, member->getState());
        BSONElement elt;
        ASSERT_TRUE(member->getFieldDotted("foo", &elt));
        ASSERT_EQUALS(15, elt.numberInt());

        // Now, finish up the AND.  Since foo == bar, we would have 11 results, but we subtract
        // one because of a mid-plan invalidation, so 10.
        int count = 0;
        while (!ah->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState status = ah->work(&id);
            if (PlanStage::ADVANCED != status) {
                continue;
            }

            ++count;
            member = ws.get(id);

            ASSERT_TRUE(member->getFieldDotted("foo", &elt));
            ASSERT_LESS_THAN_OR_EQUALS(elt.numberInt(), 20);
            ASSERT_NOT_EQUALS(15, elt.numberInt());
            ASSERT_TRUE(member->getFieldDotted("bar", &elt));
            ASSERT_GREATER_THAN_OR_EQUALS(elt.numberInt(), 10);
        }

        ASSERT_EQUALS(10, count);
    }
};

// Invalidate one of the "are we EOF?" lookahead results.
class QueryStageAndHashInvalidateLookahead : public QueryStageAndBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, ns());
        Database* db = ctx.db();
        Collection* coll = ctx.getCollection();
        if (!coll) {
            WriteUnitOfWork wuow(&_txn);
            coll = db->createCollection(&_txn, ns());
            wuow.commit();
        }

        for (int i = 0; i < 50; ++i) {
            insert(BSON("_id" << i << "foo" << i << "bar" << i << "baz" << i));
        }

        addIndex(BSON("foo" << 1));
        addIndex(BSON("bar" << 1));
        addIndex(BSON("baz" << 1));

        WorkingSet ws;
        auto ah = make_unique<AndHashStage>(&_txn, &ws, coll);

        // Foo <= 20 (descending)
        IndexScanParams params;
        params.descriptor = getIndex(BSON("foo" << 1), coll);
        params.bounds.isSimpleRange = true;
        params.bounds.startKey = BSON("" << 20);
        params.bounds.endKey = BSONObj();
        params.bounds.endKeyInclusive = true;
        params.direction = -1;
        ah->addChild(new IndexScan(&_txn, params, &ws, NULL));

        // Bar <= 19 (descending)
        params.descriptor = getIndex(BSON("bar" << 1), coll);
        params.bounds.startKey = BSON("" << 19);
        ah->addChild(new IndexScan(&_txn, params, &ws, NULL));

        // First call to work reads the first result from the children.
        // The first result is for the first scan over foo is {foo: 20, bar: 20, baz: 20}.
        // The first result is for the second scan over bar is {foo: 19, bar: 19, baz: 19}.
        WorkingSetID id = WorkingSet::INVALID_ID;
        PlanStage::StageState status = ah->work(&id);
        ASSERT_EQUALS(PlanStage::NEED_TIME, status);

        const unordered_set<WorkingSetID>& flagged = ws.getFlagged();
        ASSERT_EQUALS(size_t(0), flagged.size());

        // "delete" deletedObj (by invalidating the RecordId of the obj that matches it).
        BSONObj deletedObj = BSON("_id" << 20 << "foo" << 20 << "bar" << 20 << "baz" << 20);
        ah->saveState();
        set<RecordId> data;
        getRecordIds(&data, coll);

        size_t memUsageBefore = ah->getMemUsage();
        for (set<RecordId>::const_iterator it = data.begin(); it != data.end(); ++it) {
            if (0 == deletedObj.woCompare(coll->docFor(&_txn, *it).value())) {
                ah->invalidate(&_txn, *it, INVALIDATION_DELETION);
                break;
            }
        }

        size_t memUsageAfter = ah->getMemUsage();
        // Look ahead results do not count towards memory usage.
        ASSERT_EQUALS(memUsageBefore, memUsageAfter);

        ah->restoreState();

        // The deleted obj should show up in flagged.
        ASSERT_EQUALS(size_t(1), flagged.size());

        // And not in our results.
        int count = 0;
        while (!ah->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState status = ah->work(&id);
            if (PlanStage::ADVANCED != status) {
                continue;
            }
            WorkingSetMember* wsm = ws.get(id);
            ASSERT_NOT_EQUALS(0, deletedObj.woCompare(coll->docFor(&_txn, wsm->recordId).value()));
            ++count;
        }

        ASSERT_EQUALS(count, 20);
    }
};

// An AND with two children.
class QueryStageAndHashTwoLeaf : public QueryStageAndBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, ns());
        Database* db = ctx.db();
        Collection* coll = ctx.getCollection();
        if (!coll) {
            WriteUnitOfWork wuow(&_txn);
            coll = db->createCollection(&_txn, ns());
            wuow.commit();
        }

        for (int i = 0; i < 50; ++i) {
            insert(BSON("foo" << i << "bar" << i));
        }

        addIndex(BSON("foo" << 1));
        addIndex(BSON("bar" << 1));

        WorkingSet ws;
        auto ah = make_unique<AndHashStage>(&_txn, &ws, coll);

        // Foo <= 20
        IndexScanParams params;
        params.descriptor = getIndex(BSON("foo" << 1), coll);
        params.bounds.isSimpleRange = true;
        params.bounds.startKey = BSON("" << 20);
        params.bounds.endKey = BSONObj();
        params.bounds.endKeyInclusive = true;
        params.direction = -1;
        ah->addChild(new IndexScan(&_txn, params, &ws, NULL));

        // Bar >= 10
        params.descriptor = getIndex(BSON("bar" << 1), coll);
        params.bounds.startKey = BSON("" << 10);
        params.bounds.endKey = BSONObj();
        params.bounds.endKeyInclusive = true;
        params.direction = 1;
        ah->addChild(new IndexScan(&_txn, params, &ws, NULL));

        // foo == bar == baz, and foo<=20, bar>=10, so our values are:
        // foo == 10, 11, 12, 13, 14, 15. 16, 17, 18, 19, 20
        ASSERT_EQUALS(11, countResults(ah.get()));
    }
};

// An AND with two children.
// Add large keys (512 bytes) to index of first child to cause
// internal buffer within hashed AND to exceed threshold (32MB)
// before gathering all requested results.
class QueryStageAndHashTwoLeafFirstChildLargeKeys : public QueryStageAndBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, ns());
        Database* db = ctx.db();
        Collection* coll = ctx.getCollection();
        if (!coll) {
            WriteUnitOfWork wuow(&_txn);
            coll = db->createCollection(&_txn, ns());
            wuow.commit();
        }

        // Generate large keys for {foo: 1, big: 1} index.
        std::string big(512, 'a');
        for (int i = 0; i < 50; ++i) {
            insert(BSON("foo" << i << "bar" << i << "big" << big));
        }

        addIndex(BSON("foo" << 1 << "big" << 1));
        addIndex(BSON("bar" << 1));

        // Lower buffer limit to 20 * sizeof(big) to force memory error
        // before hashed AND is done reading the first child (stage has to
        // hold 21 keys in buffer for Foo <= 20).
        WorkingSet ws;
        auto ah = make_unique<AndHashStage>(&_txn, &ws, coll, 20 * big.size());

        // Foo <= 20
        IndexScanParams params;
        params.descriptor = getIndex(BSON("foo" << 1 << "big" << 1), coll);
        params.bounds.isSimpleRange = true;
        params.bounds.startKey = BSON("" << 20 << "" << big);
        params.bounds.endKey = BSONObj();
        params.bounds.endKeyInclusive = true;
        params.direction = -1;
        ah->addChild(new IndexScan(&_txn, params, &ws, NULL));

        // Bar >= 10
        params.descriptor = getIndex(BSON("bar" << 1), coll);
        params.bounds.startKey = BSON("" << 10);
        params.bounds.endKey = BSONObj();
        params.bounds.endKeyInclusive = true;
        params.direction = 1;
        ah->addChild(new IndexScan(&_txn, params, &ws, NULL));

        // Stage execution should fail.
        ASSERT_EQUALS(-1, countResults(ah.get()));
    }
};

// An AND with three children.
// Add large keys (512 bytes) to index of last child to verify that
// keys in last child are not buffered
class QueryStageAndHashTwoLeafLastChildLargeKeys : public QueryStageAndBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, ns());
        Database* db = ctx.db();
        Collection* coll = ctx.getCollection();
        if (!coll) {
            WriteUnitOfWork wuow(&_txn);
            coll = db->createCollection(&_txn, ns());
            wuow.commit();
        }

        // Generate large keys for {baz: 1, big: 1} index.
        std::string big(512, 'a');
        for (int i = 0; i < 50; ++i) {
            insert(BSON("foo" << i << "bar" << i << "big" << big));
        }

        addIndex(BSON("foo" << 1));
        addIndex(BSON("bar" << 1 << "big" << 1));

        // Lower buffer limit to 5 * sizeof(big) to ensure that
        // keys in last child's index are not buffered. There are 6 keys
        // that satisfy the criteria Foo <= 20 and Bar >= 10 and 5 <= baz <= 15.
        WorkingSet ws;
        auto ah = make_unique<AndHashStage>(&_txn, &ws, coll, 5 * big.size());

        // Foo <= 20
        IndexScanParams params;
        params.descriptor = getIndex(BSON("foo" << 1), coll);
        params.bounds.isSimpleRange = true;
        params.bounds.startKey = BSON("" << 20);
        params.bounds.endKey = BSONObj();
        params.bounds.endKeyInclusive = true;
        params.direction = -1;
        ah->addChild(new IndexScan(&_txn, params, &ws, NULL));

        // Bar >= 10
        params.descriptor = getIndex(BSON("bar" << 1 << "big" << 1), coll);
        params.bounds.startKey = BSON("" << 10 << "" << big);
        params.bounds.endKey = BSONObj();
        params.bounds.endKeyInclusive = true;
        params.direction = 1;
        ah->addChild(new IndexScan(&_txn, params, &ws, NULL));

        // foo == bar == baz, and foo<=20, bar>=10, so our values are:
        // foo == 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20.
        ASSERT_EQUALS(11, countResults(ah.get()));
    }
};

// An AND with three children.
class QueryStageAndHashThreeLeaf : public QueryStageAndBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, ns());
        Database* db = ctx.db();
        Collection* coll = ctx.getCollection();
        if (!coll) {
            WriteUnitOfWork wuow(&_txn);
            coll = db->createCollection(&_txn, ns());
            wuow.commit();
        }

        for (int i = 0; i < 50; ++i) {
            insert(BSON("foo" << i << "bar" << i << "baz" << i));
        }

        addIndex(BSON("foo" << 1));
        addIndex(BSON("bar" << 1));
        addIndex(BSON("baz" << 1));

        WorkingSet ws;
        auto ah = make_unique<AndHashStage>(&_txn, &ws, coll);

        // Foo <= 20
        IndexScanParams params;
        params.descriptor = getIndex(BSON("foo" << 1), coll);
        params.bounds.isSimpleRange = true;
        params.bounds.startKey = BSON("" << 20);
        params.bounds.endKey = BSONObj();
        params.bounds.endKeyInclusive = true;
        params.direction = -1;
        ah->addChild(new IndexScan(&_txn, params, &ws, NULL));

        // Bar >= 10
        params.descriptor = getIndex(BSON("bar" << 1), coll);
        params.bounds.startKey = BSON("" << 10);
        params.bounds.endKey = BSONObj();
        params.bounds.endKeyInclusive = true;
        params.direction = 1;
        ah->addChild(new IndexScan(&_txn, params, &ws, NULL));

        // 5 <= baz <= 15
        params.descriptor = getIndex(BSON("baz" << 1), coll);
        params.bounds.startKey = BSON("" << 5);
        params.bounds.endKey = BSON("" << 15);
        params.bounds.endKeyInclusive = true;
        params.direction = 1;
        ah->addChild(new IndexScan(&_txn, params, &ws, NULL));

        // foo == bar == baz, and foo<=20, bar>=10, 5<=baz<=15, so our values are:
        // foo == 10, 11, 12, 13, 14, 15.
        ASSERT_EQUALS(6, countResults(ah.get()));
    }
};

// An AND with three children.
// Add large keys (512 bytes) to index of second child to cause
// internal buffer within hashed AND to exceed threshold (32MB)
// before gathering all requested results.
// We need 3 children because the hashed AND stage buffered data for
// N-1 of its children. If the second child is the last child, it will not
// be buffered.
class QueryStageAndHashThreeLeafMiddleChildLargeKeys : public QueryStageAndBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, ns());
        Database* db = ctx.db();
        Collection* coll = ctx.getCollection();
        if (!coll) {
            WriteUnitOfWork wuow(&_txn);
            coll = db->createCollection(&_txn, ns());
            wuow.commit();
        }

        // Generate large keys for {bar: 1, big: 1} index.
        std::string big(512, 'a');
        for (int i = 0; i < 50; ++i) {
            insert(BSON("foo" << i << "bar" << i << "baz" << i << "big" << big));
        }

        addIndex(BSON("foo" << 1));
        addIndex(BSON("bar" << 1 << "big" << 1));
        addIndex(BSON("baz" << 1));

        // Lower buffer limit to 10 * sizeof(big) to force memory error
        // before hashed AND is done reading the second child (stage has to
        // hold 11 keys in buffer for Foo <= 20 and Bar >= 10).
        WorkingSet ws;
        auto ah = make_unique<AndHashStage>(&_txn, &ws, coll, 10 * big.size());

        // Foo <= 20
        IndexScanParams params;
        params.descriptor = getIndex(BSON("foo" << 1), coll);
        params.bounds.isSimpleRange = true;
        params.bounds.startKey = BSON("" << 20);
        params.bounds.endKey = BSONObj();
        params.bounds.endKeyInclusive = true;
        params.direction = -1;
        ah->addChild(new IndexScan(&_txn, params, &ws, NULL));

        // Bar >= 10
        params.descriptor = getIndex(BSON("bar" << 1 << "big" << 1), coll);
        params.bounds.startKey = BSON("" << 10 << "" << big);
        params.bounds.endKey = BSONObj();
        params.bounds.endKeyInclusive = true;
        params.direction = 1;
        ah->addChild(new IndexScan(&_txn, params, &ws, NULL));

        // 5 <= baz <= 15
        params.descriptor = getIndex(BSON("baz" << 1), coll);
        params.bounds.startKey = BSON("" << 5);
        params.bounds.endKey = BSON("" << 15);
        params.bounds.endKeyInclusive = true;
        params.direction = 1;
        ah->addChild(new IndexScan(&_txn, params, &ws, NULL));

        // Stage execution should fail.
        ASSERT_EQUALS(-1, countResults(ah.get()));
    }
};

// An AND with an index scan that returns nothing.
class QueryStageAndHashWithNothing : public QueryStageAndBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, ns());
        Database* db = ctx.db();
        Collection* coll = ctx.getCollection();
        if (!coll) {
            WriteUnitOfWork wuow(&_txn);
            coll = db->createCollection(&_txn, ns());
            wuow.commit();
        }

        for (int i = 0; i < 50; ++i) {
            insert(BSON("foo" << i << "bar" << 20));
        }

        addIndex(BSON("foo" << 1));
        addIndex(BSON("bar" << 1));

        WorkingSet ws;
        auto ah = make_unique<AndHashStage>(&_txn, &ws, coll);

        // Foo <= 20
        IndexScanParams params;
        params.descriptor = getIndex(BSON("foo" << 1), coll);
        params.bounds.isSimpleRange = true;
        params.bounds.startKey = BSON("" << 20);
        params.bounds.endKey = BSONObj();
        params.bounds.endKeyInclusive = true;
        params.direction = -1;
        ah->addChild(new IndexScan(&_txn, params, &ws, NULL));

        // Bar == 5.  Index scan should be eof.
        params.descriptor = getIndex(BSON("bar" << 1), coll);
        params.bounds.startKey = BSON("" << 5);
        params.bounds.endKey = BSON("" << 5);
        params.bounds.endKeyInclusive = true;
        params.direction = 1;
        ah->addChild(new IndexScan(&_txn, params, &ws, NULL));

        int count = 0;
        int works = 0;
        while (!ah->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            ++works;
            PlanStage::StageState status = ah->work(&id);
            if (PlanStage::ADVANCED != status) {
                continue;
            }
            ++count;
        }

        ASSERT_EQUALS(0, count);

        // We check the "look ahead for EOF" here by examining the number of works required to
        // hit EOF.  Our first call to work will pick up that bar==5 is EOF and the AND will EOF
        // immediately.
        ASSERT_EQUALS(works, 1);
    }
};

// An AND that scans data but returns nothing.
class QueryStageAndHashProducesNothing : public QueryStageAndBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, ns());
        Database* db = ctx.db();
        Collection* coll = ctx.getCollection();
        if (!coll) {
            WriteUnitOfWork wuow(&_txn);
            coll = db->createCollection(&_txn, ns());
            wuow.commit();
        }

        for (int i = 0; i < 10; ++i) {
            insert(BSON("foo" << (100 + i)));
            insert(BSON("bar" << i));
        }

        addIndex(BSON("foo" << 1));
        addIndex(BSON("bar" << 1));

        WorkingSet ws;
        auto ah = make_unique<AndHashStage>(&_txn, &ws, coll);

        // Foo >= 100
        IndexScanParams params;
        params.descriptor = getIndex(BSON("foo" << 1), coll);
        params.bounds.isSimpleRange = true;
        params.bounds.startKey = BSON("" << 100);
        params.bounds.endKey = BSONObj();
        params.bounds.endKeyInclusive = true;
        params.direction = 1;
        ah->addChild(new IndexScan(&_txn, params, &ws, NULL));

        // Bar <= 100
        params.descriptor = getIndex(BSON("bar" << 1), coll);
        params.bounds.startKey = BSON("" << 100);
        // This is subtle and confusing.  We couldn't extract any keys from the elements with
        // 'foo' in them so we would normally index them with the "nothing found" key.  We don't
        // want to include that in our scan.
        params.bounds.endKey = BSON(""
                                    << "");
        params.bounds.endKeyInclusive = false;
        params.direction = -1;
        ah->addChild(new IndexScan(&_txn, params, &ws, NULL));

        ASSERT_EQUALS(0, countResults(ah.get()));
    }
};

/**
 * SERVER-14607: Check that hash-based intersection works when the first
 * child returns fetched docs but the second child returns index keys.
 */
class QueryStageAndHashFirstChildFetched : public QueryStageAndBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, ns());
        Database* db = ctx.db();
        Collection* coll = ctx.getCollection();
        if (!coll) {
            WriteUnitOfWork wuow(&_txn);
            coll = db->createCollection(&_txn, ns());
            wuow.commit();
        }

        for (int i = 0; i < 50; ++i) {
            insert(BSON("foo" << i << "bar" << i));
        }

        addIndex(BSON("foo" << 1));
        addIndex(BSON("bar" << 1));

        WorkingSet ws;
        auto ah = make_unique<AndHashStage>(&_txn, &ws, coll);

        // Foo <= 20
        IndexScanParams params;
        params.descriptor = getIndex(BSON("foo" << 1), coll);
        params.bounds.isSimpleRange = true;
        params.bounds.startKey = BSON("" << 20);
        params.bounds.endKey = BSONObj();
        params.bounds.endKeyInclusive = true;
        params.direction = -1;
        IndexScan* firstScan = new IndexScan(&_txn, params, &ws, NULL);

        // First child of the AND_HASH stage is a Fetch. The NULL in the
        // constructor means there is no filter.
        FetchStage* fetch = new FetchStage(&_txn, &ws, firstScan, NULL, coll);
        ah->addChild(fetch);

        // Bar >= 10
        params.descriptor = getIndex(BSON("bar" << 1), coll);
        params.bounds.startKey = BSON("" << 10);
        params.bounds.endKey = BSONObj();
        params.bounds.endKeyInclusive = true;
        params.direction = 1;
        ah->addChild(new IndexScan(&_txn, params, &ws, NULL));

        // Check that the AndHash stage returns docs {foo: 10, bar: 10}
        // through {foo: 20, bar: 20}.
        for (int i = 10; i <= 20; i++) {
            BSONObj obj = getNext(ah.get(), &ws);
            ASSERT_EQUALS(i, obj["foo"].numberInt());
            ASSERT_EQUALS(i, obj["bar"].numberInt());
        }
    }
};

/**
 * SERVER-14607: Check that hash-based intersection works when the first
 * child returns index keys but the second returns fetched docs.
 */
class QueryStageAndHashSecondChildFetched : public QueryStageAndBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, ns());
        Database* db = ctx.db();
        Collection* coll = ctx.getCollection();
        if (!coll) {
            WriteUnitOfWork wuow(&_txn);
            coll = db->createCollection(&_txn, ns());
            wuow.commit();
        }

        for (int i = 0; i < 50; ++i) {
            insert(BSON("foo" << i << "bar" << i));
        }

        addIndex(BSON("foo" << 1));
        addIndex(BSON("bar" << 1));

        WorkingSet ws;
        auto ah = make_unique<AndHashStage>(&_txn, &ws, coll);

        // Foo <= 20
        IndexScanParams params;
        params.descriptor = getIndex(BSON("foo" << 1), coll);
        params.bounds.isSimpleRange = true;
        params.bounds.startKey = BSON("" << 20);
        params.bounds.endKey = BSONObj();
        params.bounds.endKeyInclusive = true;
        params.direction = -1;
        ah->addChild(new IndexScan(&_txn, params, &ws, NULL));

        // Bar >= 10
        params.descriptor = getIndex(BSON("bar" << 1), coll);
        params.bounds.startKey = BSON("" << 10);
        params.bounds.endKey = BSONObj();
        params.bounds.endKeyInclusive = true;
        params.direction = 1;
        IndexScan* secondScan = new IndexScan(&_txn, params, &ws, NULL);

        // Second child of the AND_HASH stage is a Fetch. The NULL in the
        // constructor means there is no filter.
        FetchStage* fetch = new FetchStage(&_txn, &ws, secondScan, NULL, coll);
        ah->addChild(fetch);

        // Check that the AndHash stage returns docs {foo: 10, bar: 10}
        // through {foo: 20, bar: 20}.
        for (int i = 10; i <= 20; i++) {
            BSONObj obj = getNext(ah.get(), &ws);
            ASSERT_EQUALS(i, obj["foo"].numberInt());
            ASSERT_EQUALS(i, obj["bar"].numberInt());
        }
    }
};


class QueryStageAndHashDeadChild : public QueryStageAndBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, ns());
        Database* db = ctx.db();
        Collection* coll = ctx.getCollection();
        if (!coll) {
            WriteUnitOfWork wuow(&_txn);
            coll = db->createCollection(&_txn, ns());
            wuow.commit();
        }

        const BSONObj dataObj = fromjson("{'foo': 'bar'}");

        // Confirm PlanStage::DEAD when children contain the following WorkingSetMembers:
        //     Child1:  Data
        //     Child2:  NEED_TIME, DEAD
        {
            WorkingSet ws;
            const auto andHashStage = make_unique<AndHashStage>(&_txn, &ws, coll);

            auto childStage1 = make_unique<QueuedDataStage>(&_txn, &ws);
            {
                WorkingSetID id = ws.allocate();
                WorkingSetMember* wsm = ws.get(id);
                wsm->recordId = RecordId(1);
                wsm->obj = Snapshotted<BSONObj>(SnapshotId(), dataObj);
                ws.transitionToRecordIdAndObj(id);
                childStage1->pushBack(id);
            }

            auto childStage2 = make_unique<QueuedDataStage>(&_txn, &ws);
            childStage2->pushBack(PlanStage::NEED_TIME);
            childStage2->pushBack(PlanStage::DEAD);

            andHashStage->addChild(childStage1.release());
            andHashStage->addChild(childStage2.release());

            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState state = PlanStage::NEED_TIME;
            while (PlanStage::NEED_TIME == state) {
                state = andHashStage->work(&id);
            }

            ASSERT_EQ(PlanStage::DEAD, state);
        }

        // Confirm PlanStage::DEAD when children contain the following WorkingSetMembers:
        //     Child1:  Data, DEAD
        //     Child2:  Data
        {
            WorkingSet ws;
            const auto andHashStage = make_unique<AndHashStage>(&_txn, &ws, coll);

            auto childStage1 = make_unique<QueuedDataStage>(&_txn, &ws);

            {
                WorkingSetID id = ws.allocate();
                WorkingSetMember* wsm = ws.get(id);
                wsm->recordId = RecordId(1);
                wsm->obj = Snapshotted<BSONObj>(SnapshotId(), dataObj);
                ws.transitionToRecordIdAndObj(id);
                childStage1->pushBack(id);
            }
            childStage1->pushBack(PlanStage::DEAD);

            auto childStage2 = make_unique<QueuedDataStage>(&_txn, &ws);
            {
                WorkingSetID id = ws.allocate();
                WorkingSetMember* wsm = ws.get(id);
                wsm->recordId = RecordId(2);
                wsm->obj = Snapshotted<BSONObj>(SnapshotId(), dataObj);
                ws.transitionToRecordIdAndObj(id);
                childStage2->pushBack(id);
            }

            andHashStage->addChild(childStage1.release());
            andHashStage->addChild(childStage2.release());

            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState state = PlanStage::NEED_TIME;
            while (PlanStage::NEED_TIME == state) {
                state = andHashStage->work(&id);
            }

            ASSERT_EQ(PlanStage::DEAD, state);
        }

        // Confirm PlanStage::DEAD when children contain the following WorkingSetMembers:
        //     Child1:  Data
        //     Child2:  Data, DEAD
        {
            WorkingSet ws;
            const auto andHashStage = make_unique<AndHashStage>(&_txn, &ws, coll);

            auto childStage1 = make_unique<QueuedDataStage>(&_txn, &ws);
            {
                WorkingSetID id = ws.allocate();
                WorkingSetMember* wsm = ws.get(id);
                wsm->recordId = RecordId(1);
                wsm->obj = Snapshotted<BSONObj>(SnapshotId(), dataObj);
                ws.transitionToRecordIdAndObj(id);
                childStage1->pushBack(id);
            }

            auto childStage2 = make_unique<QueuedDataStage>(&_txn, &ws);
            {
                WorkingSetID id = ws.allocate();
                WorkingSetMember* wsm = ws.get(id);
                wsm->recordId = RecordId(2);
                wsm->obj = Snapshotted<BSONObj>(SnapshotId(), dataObj);
                ws.transitionToRecordIdAndObj(id);
                childStage2->pushBack(id);
            }
            childStage2->pushBack(PlanStage::DEAD);

            andHashStage->addChild(childStage1.release());
            andHashStage->addChild(childStage2.release());

            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState state = PlanStage::NEED_TIME;
            while (PlanStage::NEED_TIME == state) {
                state = andHashStage->work(&id);
            }

            ASSERT_EQ(PlanStage::DEAD, state);
        }
    }
};

//
// Sorted AND tests
//

/**
 * Invalidate a RecordId held by a sorted AND before the AND finishes evaluating.  The AND should
 * process all other data just fine and flag the invalidated RecordId in the WorkingSet.
 */
class QueryStageAndSortedInvalidation : public QueryStageAndBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, ns());
        Database* db = ctx.db();
        Collection* coll = ctx.getCollection();
        if (!coll) {
            WriteUnitOfWork wuow(&_txn);
            coll = db->createCollection(&_txn, ns());
            wuow.commit();
        }

        // Insert a bunch of data
        for (int i = 0; i < 50; ++i) {
            insert(BSON("foo" << 1 << "bar" << 1));
        }
        addIndex(BSON("foo" << 1));
        addIndex(BSON("bar" << 1));

        WorkingSet ws;
        auto ah = make_unique<AndSortedStage>(&_txn, &ws, coll);

        // Scan over foo == 1
        IndexScanParams params;
        params.descriptor = getIndex(BSON("foo" << 1), coll);
        params.bounds.isSimpleRange = true;
        params.bounds.startKey = BSON("" << 1);
        params.bounds.endKey = BSON("" << 1);
        params.bounds.endKeyInclusive = true;
        params.direction = 1;
        ah->addChild(new IndexScan(&_txn, params, &ws, NULL));

        // Scan over bar == 1
        params.descriptor = getIndex(BSON("bar" << 1), coll);
        ah->addChild(new IndexScan(&_txn, params, &ws, NULL));

        // Get the set of RecordIds in our collection to use later.
        set<RecordId> data;
        getRecordIds(&data, coll);

        // We're making an assumption here that happens to be true because we clear out the
        // collection before running this: increasing inserts have increasing RecordIds.
        // This isn't true in general if the collection is not dropped beforehand.
        WorkingSetID id = WorkingSet::INVALID_ID;

        // Sorted AND looks at the first child, which is an index scan over foo==1.
        ah->work(&id);

        // The first thing that the index scan returns (due to increasing RecordId trick) is the
        // very first insert, which should be the very first thing in data.  Let's invalidate it
        // and make sure it shows up in the flagged results.
        ah->saveState();
        ah->invalidate(&_txn, *data.begin(), INVALIDATION_DELETION);
        remove(coll->docFor(&_txn, *data.begin()).value());
        ah->restoreState();

        // Make sure the nuked obj is actually in the flagged data.
        ASSERT_EQUALS(ws.getFlagged().size(), size_t(1));
        WorkingSetMember* member = ws.get(*ws.getFlagged().begin());
        ASSERT_EQUALS(WorkingSetMember::OWNED_OBJ, member->getState());
        BSONElement elt;
        ASSERT_TRUE(member->getFieldDotted("foo", &elt));
        ASSERT_EQUALS(1, elt.numberInt());
        ASSERT_TRUE(member->getFieldDotted("bar", &elt));
        ASSERT_EQUALS(1, elt.numberInt());

        set<RecordId>::iterator it = data.begin();

        // Proceed along, AND-ing results.
        int count = 0;
        while (!ah->isEOF() && count < 10) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState status = ah->work(&id);
            if (PlanStage::ADVANCED != status) {
                continue;
            }

            ++count;
            ++it;
            member = ws.get(id);

            ASSERT_TRUE(member->getFieldDotted("foo", &elt));
            ASSERT_EQUALS(1, elt.numberInt());
            ASSERT_TRUE(member->getFieldDotted("bar", &elt));
            ASSERT_EQUALS(1, elt.numberInt());
            ASSERT_EQUALS(member->recordId, *it);
        }

        // Move 'it' to a result that's yet to show up.
        for (int i = 0; i < count + 10; ++i) {
            ++it;
        }
        // Remove a result that's coming up.  It's not the 'target' result of the AND so it's
        // not flagged.
        ah->saveState();
        ah->invalidate(&_txn, *it, INVALIDATION_DELETION);
        remove(coll->docFor(&_txn, *it).value());
        ah->restoreState();

        // Get all results aside from the two we killed.
        while (!ah->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState status = ah->work(&id);
            if (PlanStage::ADVANCED != status) {
                continue;
            }

            ++count;
            member = ws.get(id);

            ASSERT_TRUE(member->getFieldDotted("foo", &elt));
            ASSERT_EQUALS(1, elt.numberInt());
            ASSERT_TRUE(member->getFieldDotted("bar", &elt));
            ASSERT_EQUALS(1, elt.numberInt());
        }

        ASSERT_EQUALS(count, 48);

        ASSERT_EQUALS(size_t(1), ws.getFlagged().size());
    }
};


// An AND with three children.
class QueryStageAndSortedThreeLeaf : public QueryStageAndBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, ns());
        Database* db = ctx.db();
        Collection* coll = ctx.getCollection();
        if (!coll) {
            WriteUnitOfWork wuow(&_txn);
            coll = db->createCollection(&_txn, ns());
            wuow.commit();
        }

        // Insert a bunch of data
        for (int i = 0; i < 50; ++i) {
            // Some data that'll show up but not be in all.
            insert(BSON("foo" << 1 << "baz" << 1));
            insert(BSON("foo" << 1 << "bar" << 1));
            // The needle in the haystack.  Only these should be returned by the AND.
            insert(BSON("foo" << 1 << "bar" << 1 << "baz" << 1));
            insert(BSON("foo" << 1));
            insert(BSON("bar" << 1));
            insert(BSON("baz" << 1));
        }

        addIndex(BSON("foo" << 1));
        addIndex(BSON("bar" << 1));
        addIndex(BSON("baz" << 1));

        WorkingSet ws;
        auto ah = make_unique<AndSortedStage>(&_txn, &ws, coll);

        // Scan over foo == 1
        IndexScanParams params;
        params.descriptor = getIndex(BSON("foo" << 1), coll);
        params.bounds.isSimpleRange = true;
        params.bounds.startKey = BSON("" << 1);
        params.bounds.endKey = BSON("" << 1);
        params.bounds.endKeyInclusive = true;
        params.direction = 1;
        ah->addChild(new IndexScan(&_txn, params, &ws, NULL));

        // bar == 1
        params.descriptor = getIndex(BSON("bar" << 1), coll);
        ah->addChild(new IndexScan(&_txn, params, &ws, NULL));

        // baz == 1
        params.descriptor = getIndex(BSON("baz" << 1), coll);
        ah->addChild(new IndexScan(&_txn, params, &ws, NULL));

        ASSERT_EQUALS(50, countResults(ah.get()));
    }
};

// An AND with an index scan that returns nothing.
class QueryStageAndSortedWithNothing : public QueryStageAndBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, ns());
        Database* db = ctx.db();
        Collection* coll = ctx.getCollection();
        if (!coll) {
            WriteUnitOfWork wuow(&_txn);
            coll = db->createCollection(&_txn, ns());
            wuow.commit();
        }

        for (int i = 0; i < 50; ++i) {
            insert(BSON("foo" << 8 << "bar" << 20));
        }

        addIndex(BSON("foo" << 1));
        addIndex(BSON("bar" << 1));

        WorkingSet ws;
        auto ah = make_unique<AndSortedStage>(&_txn, &ws, coll);

        // Foo == 7.  Should be EOF.
        IndexScanParams params;
        params.descriptor = getIndex(BSON("foo" << 1), coll);
        params.bounds.isSimpleRange = true;
        params.bounds.startKey = BSON("" << 7);
        params.bounds.endKey = BSON("" << 7);
        params.bounds.endKeyInclusive = true;
        params.direction = 1;
        ah->addChild(new IndexScan(&_txn, params, &ws, NULL));

        // Bar == 20, not EOF.
        params.descriptor = getIndex(BSON("bar" << 1), coll);
        params.bounds.startKey = BSON("" << 20);
        params.bounds.endKey = BSON("" << 20);
        params.bounds.endKeyInclusive = true;
        params.direction = 1;
        ah->addChild(new IndexScan(&_txn, params, &ws, NULL));

        ASSERT_EQUALS(0, countResults(ah.get()));
    }
};

// An AND that scans data but returns nothing.
class QueryStageAndSortedProducesNothing : public QueryStageAndBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, ns());
        Database* db = ctx.db();
        Collection* coll = ctx.getCollection();
        if (!coll) {
            WriteUnitOfWork wuow(&_txn);
            coll = db->createCollection(&_txn, ns());
            wuow.commit();
        }

        for (int i = 0; i < 50; ++i) {
            // Insert data with foo=7, bar==20, but nothing with both.
            insert(BSON("foo" << 8 << "bar" << 20));
            insert(BSON("foo" << 7 << "bar" << 21));
            insert(BSON("foo" << 7));
            insert(BSON("bar" << 20));
        }

        addIndex(BSON("foo" << 1));
        addIndex(BSON("bar" << 1));

        WorkingSet ws;
        auto ah = make_unique<AndSortedStage>(&_txn, &ws, coll);

        // foo == 7.
        IndexScanParams params;
        params.descriptor = getIndex(BSON("foo" << 1), coll);
        params.bounds.isSimpleRange = true;
        params.bounds.startKey = BSON("" << 7);
        params.bounds.endKey = BSON("" << 7);
        params.bounds.endKeyInclusive = true;
        params.direction = 1;
        ah->addChild(new IndexScan(&_txn, params, &ws, NULL));

        // bar == 20.
        params.descriptor = getIndex(BSON("bar" << 1), coll);
        params.bounds.startKey = BSON("" << 20);
        params.bounds.endKey = BSON("" << 20);
        params.bounds.endKeyInclusive = true;
        params.direction = 1;
        ah->addChild(new IndexScan(&_txn, params, &ws, NULL));

        ASSERT_EQUALS(0, countResults(ah.get()));
    }
};

// Verify that AND preserves the order of the last child.
class QueryStageAndSortedByLastChild : public QueryStageAndBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, ns());
        Database* db = ctx.db();
        Collection* coll = ctx.getCollection();
        if (!coll) {
            WriteUnitOfWork wuow(&_txn);
            coll = db->createCollection(&_txn, ns());
            wuow.commit();
        }

        for (int i = 0; i < 50; ++i) {
            insert(BSON("foo" << 1 << "bar" << i));
        }

        addIndex(BSON("foo" << 1));
        addIndex(BSON("bar" << 1));

        WorkingSet ws;
        auto ah = make_unique<AndHashStage>(&_txn, &ws, coll);

        // Scan over foo == 1
        IndexScanParams params;
        params.descriptor = getIndex(BSON("foo" << 1), coll);
        params.bounds.isSimpleRange = true;
        params.bounds.startKey = BSON("" << 1);
        params.bounds.endKey = BSON("" << 1);
        params.bounds.endKeyInclusive = true;
        params.direction = 1;
        ah->addChild(new IndexScan(&_txn, params, &ws, NULL));

        // Intersect with 7 <= bar < 10000
        params.descriptor = getIndex(BSON("bar" << 1), coll);
        params.bounds.startKey = BSON("" << 7);
        params.bounds.endKey = BSON("" << 10000);
        ah->addChild(new IndexScan(&_txn, params, &ws, NULL));

        WorkingSetID lastId = WorkingSet::INVALID_ID;

        int count = 0;
        while (!ah->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState status = ah->work(&id);
            if (PlanStage::ADVANCED != status) {
                continue;
            }
            BSONObj thisObj = coll->docFor(&_txn, ws.get(id)->recordId).value();
            ASSERT_EQUALS(7 + count, thisObj["bar"].numberInt());
            ++count;
            if (WorkingSet::INVALID_ID != lastId) {
                BSONObj lastObj = coll->docFor(&_txn, ws.get(lastId)->recordId).value();
                ASSERT_LESS_THAN(lastObj["bar"].woCompare(thisObj["bar"]), 0);
            }
            lastId = id;
        }

        ASSERT_EQUALS(count, 43);
    }
};

/**
 * SERVER-14607: Check that sort-based intersection works when the first
 * child returns fetched docs but the second child returns index keys.
 */
class QueryStageAndSortedFirstChildFetched : public QueryStageAndBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, ns());
        Database* db = ctx.db();
        Collection* coll = ctx.getCollection();
        if (!coll) {
            WriteUnitOfWork wuow(&_txn);
            coll = db->createCollection(&_txn, ns());
            wuow.commit();
        }

        // Insert a bunch of data
        for (int i = 0; i < 50; ++i) {
            insert(BSON("foo" << 1 << "bar" << 1));
        }

        addIndex(BSON("foo" << 1));
        addIndex(BSON("bar" << 1));

        WorkingSet ws;
        unique_ptr<AndSortedStage> as = make_unique<AndSortedStage>(&_txn, &ws, coll);

        // Scan over foo == 1
        IndexScanParams params;
        params.descriptor = getIndex(BSON("foo" << 1), coll);
        params.bounds.isSimpleRange = true;
        params.bounds.startKey = BSON("" << 1);
        params.bounds.endKey = BSON("" << 1);
        params.bounds.endKeyInclusive = true;
        params.direction = 1;
        IndexScan* firstScan = new IndexScan(&_txn, params, &ws, NULL);

        // First child of the AND_SORTED stage is a Fetch. The NULL in the
        // constructor means there is no filter.
        FetchStage* fetch = new FetchStage(&_txn, &ws, firstScan, NULL, coll);
        as->addChild(fetch);

        // bar == 1
        params.descriptor = getIndex(BSON("bar" << 1), coll);
        as->addChild(new IndexScan(&_txn, params, &ws, NULL));

        for (int i = 0; i < 50; i++) {
            BSONObj obj = getNext(as.get(), &ws);
            ASSERT_EQUALS(1, obj["foo"].numberInt());
            ASSERT_EQUALS(1, obj["bar"].numberInt());
        }
    }
};

/**
 * SERVER-14607: Check that sort-based intersection works when the first
 * child returns index keys but the second returns fetched docs.
 */
class QueryStageAndSortedSecondChildFetched : public QueryStageAndBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, ns());
        Database* db = ctx.db();
        Collection* coll = ctx.getCollection();
        if (!coll) {
            WriteUnitOfWork wuow(&_txn);
            coll = db->createCollection(&_txn, ns());
            wuow.commit();
        }

        // Insert a bunch of data
        for (int i = 0; i < 50; ++i) {
            insert(BSON("foo" << 1 << "bar" << 1));
        }

        addIndex(BSON("foo" << 1));
        addIndex(BSON("bar" << 1));

        WorkingSet ws;
        unique_ptr<AndSortedStage> as = make_unique<AndSortedStage>(&_txn, &ws, coll);

        // Scan over foo == 1
        IndexScanParams params;
        params.descriptor = getIndex(BSON("foo" << 1), coll);
        params.bounds.isSimpleRange = true;
        params.bounds.startKey = BSON("" << 1);
        params.bounds.endKey = BSON("" << 1);
        params.bounds.endKeyInclusive = true;
        params.direction = 1;
        as->addChild(new IndexScan(&_txn, params, &ws, NULL));

        // bar == 1
        params.descriptor = getIndex(BSON("bar" << 1), coll);
        IndexScan* secondScan = new IndexScan(&_txn, params, &ws, NULL);

        // Second child of the AND_SORTED stage is a Fetch. The NULL in the
        // constructor means there is no filter.
        FetchStage* fetch = new FetchStage(&_txn, &ws, secondScan, NULL, coll);
        as->addChild(fetch);

        for (int i = 0; i < 50; i++) {
            BSONObj obj = getNext(as.get(), &ws);
            ASSERT_EQUALS(1, obj["foo"].numberInt());
            ASSERT_EQUALS(1, obj["bar"].numberInt());
        }
    }
};


class All : public Suite {
public:
    All() : Suite("query_stage_and") {}

    void setupTests() {
        add<QueryStageAndHashInvalidation>();
        add<QueryStageAndHashTwoLeaf>();
        add<QueryStageAndHashTwoLeafFirstChildLargeKeys>();
        add<QueryStageAndHashTwoLeafLastChildLargeKeys>();
        add<QueryStageAndHashThreeLeaf>();
        add<QueryStageAndHashThreeLeafMiddleChildLargeKeys>();
        add<QueryStageAndHashWithNothing>();
        add<QueryStageAndHashProducesNothing>();
        add<QueryStageAndHashInvalidateLookahead>();
        add<QueryStageAndHashFirstChildFetched>();
        add<QueryStageAndHashSecondChildFetched>();
        add<QueryStageAndHashDeadChild>();
        add<QueryStageAndSortedInvalidation>();
        add<QueryStageAndSortedThreeLeaf>();
        add<QueryStageAndSortedWithNothing>();
        add<QueryStageAndSortedProducesNothing>();
        add<QueryStageAndSortedByLastChild>();
        add<QueryStageAndSortedFirstChildFetched>();
        add<QueryStageAndSortedSecondChildFetched>();
    }
};

SuiteInstance<All> queryStageAndAll;

}  // namespace QueryStageAnd
