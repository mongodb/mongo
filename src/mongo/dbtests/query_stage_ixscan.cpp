/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/dbtests/dbtests.h"

namespace QueryStageIxscan {

class IndexScanTest {
public:
    IndexScanTest()
        : _scopedXact(&_txn, MODE_IX),
          _dbLock(_txn.lockState(), nsToDatabaseSubstring(ns()), MODE_X),
          _ctx(&_txn, ns()),
          _coll(NULL) {}

    virtual ~IndexScanTest() {}

    virtual void setup() {
        WriteUnitOfWork wunit(&_txn);

        _ctx.db()->dropCollection(&_txn, ns());
        _coll = _ctx.db()->createCollection(&_txn, ns());

        ASSERT_OK(_coll->getIndexCatalog()->createIndexOnEmptyCollection(
            &_txn,
            BSON("ns" << ns() << "key" << BSON("x" << 1) << "name"
                      << DBClientBase::genIndexName(BSON("x" << 1)))));

        wunit.commit();
    }

    void insert(const BSONObj& doc) {
        WriteUnitOfWork wunit(&_txn);
        OpDebug* const nullOpDebug = nullptr;
        ASSERT_OK(_coll->insertDocument(&_txn, doc, nullOpDebug, false));
        wunit.commit();
    }

    /**
     * Works 'ixscan' until it advances. Returns the index key via a pointer to the
     * WorkingSetMember containing the key.
     */
    WorkingSetMember* getNext(IndexScan* ixscan) {
        WorkingSetID out;

        PlanStage::StageState state = PlanStage::NEED_TIME;
        while (PlanStage::ADVANCED != state) {
            state = ixscan->work(&out);

            // There are certain states we shouldn't get.
            ASSERT_NE(PlanStage::IS_EOF, state);
            ASSERT_NE(PlanStage::DEAD, state);
            ASSERT_NE(PlanStage::FAILURE, state);
        }

        return _ws.get(out);
    }


    IndexScan* createIndexScanSimpleRange(BSONObj startKey, BSONObj endKey) {
        IndexCatalog* catalog = _coll->getIndexCatalog();
        IndexDescriptor* descriptor = catalog->findIndexByKeyPattern(&_txn, BSON("x" << 1));
        invariant(descriptor);

        // We are not testing indexing here so use maximal bounds
        IndexScanParams params;
        params.descriptor = descriptor;
        params.bounds.isSimpleRange = true;
        params.bounds.startKey = startKey;
        params.bounds.endKey = endKey;
        params.bounds.endKeyInclusive = true;
        params.direction = 1;

        // This child stage gets owned and freed by the caller.
        MatchExpression* filter = NULL;
        return new IndexScan(&_txn, params, &_ws, filter);
    }

    IndexScan* createIndexScan(BSONObj startKey,
                               BSONObj endKey,
                               bool startInclusive,
                               bool endInclusive,
                               int direction = 1) {
        IndexCatalog* catalog = _coll->getIndexCatalog();
        IndexDescriptor* descriptor = catalog->findIndexByKeyPattern(&_txn, BSON("x" << 1));
        invariant(descriptor);

        IndexScanParams params;
        params.descriptor = descriptor;
        params.direction = direction;

        OrderedIntervalList oil("x");
        BSONObjBuilder bob;
        bob.appendAs(startKey.firstElement(), "");
        bob.appendAs(endKey.firstElement(), "");
        oil.intervals.push_back(Interval(bob.obj(), startInclusive, endInclusive));
        params.bounds.fields.push_back(oil);

        MatchExpression* filter = NULL;
        return new IndexScan(&_txn, params, &_ws, filter);
    }

    static const char* ns() {
        return "unittest.QueryStageIxscan";
    }

protected:
    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _txn = *_txnPtr;

    ScopedTransaction _scopedXact;
    Lock::DBLock _dbLock;
    OldClientContext _ctx;
    Collection* _coll;

    WorkingSet _ws;
};

// SERVER-15958: Some IndexScanStats info must be initialized on construction of an IndexScan.
class QueryStageIxscanInitializeStats : public IndexScanTest {
public:
    void run() {
        setup();

        // Make the {x: 1} index multikey by inserting a doc where 'x' is an array.
        insert(fromjson("{_id: 1, x: [1, 2, 3]}"));

        std::unique_ptr<IndexScan> ixscan(
            createIndexScanSimpleRange(BSON("x" << 1), BSON("x" << 3)));

        // Verify that SpecificStats of 'ixscan' have been properly initialized.
        const IndexScanStats* stats =
            static_cast<const IndexScanStats*>(ixscan->getSpecificStats());
        ASSERT(stats);
        ASSERT_TRUE(stats->isMultiKey);
        ASSERT_EQUALS(stats->keyPattern, BSON("x" << 1));
    }
};

// SERVER-16437
class QueryStageIxscanInsertDuringSave : public IndexScanTest {
public:
    void run() {
        setup();

        insert(fromjson("{_id: 1, x: 5}"));
        insert(fromjson("{_id: 2, x: 6}"));
        insert(fromjson("{_id: 3, x: 12}"));

        std::unique_ptr<IndexScan> ixscan(
            createIndexScan(BSON("x" << 5), BSON("x" << 10), true, true));

        // Expect to get key {'': 5} and then key {'': 6}.
        WorkingSetMember* member = getNext(ixscan.get());
        ASSERT_EQ(WorkingSetMember::RID_AND_IDX, member->getState());
        ASSERT_EQ(member->keyData[0].keyData, BSON("" << 5));
        member = getNext(ixscan.get());
        ASSERT_EQ(WorkingSetMember::RID_AND_IDX, member->getState());
        ASSERT_EQ(member->keyData[0].keyData, BSON("" << 6));

        // Save state and insert a few indexed docs.
        ixscan->saveState();
        insert(fromjson("{_id: 4, x: 10}"));
        insert(fromjson("{_id: 5, x: 11}"));
        ixscan->restoreState();

        member = getNext(ixscan.get());
        ASSERT_EQ(WorkingSetMember::RID_AND_IDX, member->getState());
        ASSERT_EQ(member->keyData[0].keyData, BSON("" << 10));

        WorkingSetID id;
        ASSERT_EQ(PlanStage::IS_EOF, ixscan->work(&id));
        ASSERT(ixscan->isEOF());
    }
};

// SERVER-16437
class QueryStageIxscanInsertDuringSaveExclusive : public IndexScanTest {
public:
    void run() {
        setup();

        insert(fromjson("{_id: 1, x: 5}"));
        insert(fromjson("{_id: 2, x: 6}"));
        insert(fromjson("{_id: 3, x: 10}"));

        std::unique_ptr<IndexScan> ixscan(
            createIndexScan(BSON("x" << 5), BSON("x" << 10), false, false));

        // Expect to get key {'': 6}.
        WorkingSetMember* member = getNext(ixscan.get());
        ASSERT_EQ(WorkingSetMember::RID_AND_IDX, member->getState());
        ASSERT_EQ(member->keyData[0].keyData, BSON("" << 6));

        // Save state and insert an indexed doc.
        ixscan->saveState();
        insert(fromjson("{_id: 4, x: 7}"));
        ixscan->restoreState();

        member = getNext(ixscan.get());
        ASSERT_EQ(WorkingSetMember::RID_AND_IDX, member->getState());
        ASSERT_EQ(member->keyData[0].keyData, BSON("" << 7));

        WorkingSetID id;
        ASSERT_EQ(PlanStage::IS_EOF, ixscan->work(&id));
        ASSERT(ixscan->isEOF());
    }
};

// SERVER-16437
class QueryStageIxscanInsertDuringSaveExclusive2 : public IndexScanTest {
public:
    void run() {
        setup();

        insert(fromjson("{_id: 1, x: 5}"));
        insert(fromjson("{_id: 2, x: 6}"));
        insert(fromjson("{_id: 3, x: 12}"));

        std::unique_ptr<IndexScan> ixscan(
            createIndexScan(BSON("x" << 5), BSON("x" << 10), false, false));

        // Expect to get key {'': 6}.
        WorkingSetMember* member = getNext(ixscan.get());
        ASSERT_EQ(WorkingSetMember::RID_AND_IDX, member->getState());
        ASSERT_EQ(member->keyData[0].keyData, BSON("" << 6));

        // Save state and insert an indexed doc.
        ixscan->saveState();
        insert(fromjson("{_id: 4, x: 10}"));
        ixscan->restoreState();

        // Ensure that we're EOF and we don't erroneously return {'': 12}.
        WorkingSetID id;
        ASSERT_EQ(PlanStage::IS_EOF, ixscan->work(&id));
        ASSERT(ixscan->isEOF());
    }
};

// SERVER-16437
class QueryStageIxscanInsertDuringSaveReverse : public IndexScanTest {
public:
    void run() {
        setup();

        insert(fromjson("{_id: 1, x: 10}"));
        insert(fromjson("{_id: 2, x: 8}"));
        insert(fromjson("{_id: 3, x: 3}"));

        std::unique_ptr<IndexScan> ixscan(
            createIndexScan(BSON("x" << 10), BSON("x" << 5), true, true, -1 /* reverse scan */));

        // Expect to get key {'': 10} and then {'': 8}.
        WorkingSetMember* member = getNext(ixscan.get());
        ASSERT_EQ(WorkingSetMember::RID_AND_IDX, member->getState());
        ASSERT_EQ(member->keyData[0].keyData, BSON("" << 10));
        member = getNext(ixscan.get());
        ASSERT_EQ(WorkingSetMember::RID_AND_IDX, member->getState());
        ASSERT_EQ(member->keyData[0].keyData, BSON("" << 8));

        // Save state and insert an indexed doc.
        ixscan->saveState();
        insert(fromjson("{_id: 4, x: 6}"));
        insert(fromjson("{_id: 5, x: 9}"));
        ixscan->restoreState();

        // Ensure that we don't erroneously return {'': 9} or {'':3}.
        member = getNext(ixscan.get());
        ASSERT_EQ(WorkingSetMember::RID_AND_IDX, member->getState());
        ASSERT_EQ(member->keyData[0].keyData, BSON("" << 6));

        WorkingSetID id;
        ASSERT_EQ(PlanStage::IS_EOF, ixscan->work(&id));
        ASSERT(ixscan->isEOF());
    }
};

class All : public Suite {
public:
    All() : Suite("query_stage_ixscan") {}

    void setupTests() {
        add<QueryStageIxscanInitializeStats>();
        add<QueryStageIxscanInsertDuringSave>();
        add<QueryStageIxscanInsertDuringSaveExclusive>();
        add<QueryStageIxscanInsertDuringSaveExclusive2>();
        add<QueryStageIxscanInsertDuringSaveReverse>();
    }
} QueryStageIxscanAll;

}  // namespace QueryStageIxscan
