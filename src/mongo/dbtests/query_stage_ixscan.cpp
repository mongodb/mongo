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

#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <memory>
#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/db/query/interval.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo {
namespace QueryStageIxscan {
namespace {
const auto kIndexVersion = IndexDescriptor::IndexVersion::kV2;
}  // namespace

class IndexScanTest {
public:
    IndexScanTest()
        : _dbLock(&_opCtx, nss().dbName(), MODE_X),
          _ctx(&_opCtx, nss()),
          _coll(nullptr),
          _expCtx(ExpressionContextBuilder{}.opCtx(&_opCtx).ns(nss()).build()) {}

    virtual ~IndexScanTest() {}

    virtual void setup() {
        WriteUnitOfWork wunit(&_opCtx);

        _ctx.db()->dropCollection(&_opCtx, nss()).transitional_ignore();
        _coll = _ctx.db()->createCollection(&_opCtx, nss());
        _collPtr = CollectionPtr(_coll);

        ASSERT_OK(_coll->getIndexCatalog()->createIndexOnEmptyCollection(
            &_opCtx,
            _coll,
            BSON("key" << BSON("x" << 1) << "name" << DBClientBase::genIndexName(BSON("x" << 1))
                       << "v" << static_cast<int>(kIndexVersion))));

        wunit.commit();
    }

    void insert(const BSONObj& doc) {
        WriteUnitOfWork wunit(&_opCtx);
        OpDebug* const nullOpDebug = nullptr;
        ASSERT_OK(collection_internal::insertDocument(
            &_opCtx, CollectionPtr(_coll), InsertStatement(doc), nullOpDebug, false));
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
            ASSERT_NE(PlanStage::IS_EOF, state);
        }

        return _ws.get(out);
    }

    /**
     * Works 'ixscan' until the scan ends. Asserts that no new results will be found.
     */
    void assertNoNextResult(IndexScan* ixscan) {
        WorkingSetID out;

        PlanStage::StageState state = PlanStage::NEED_TIME;
        while (PlanStage::IS_EOF != state) {
            state = ixscan->work(&out);
            ASSERT_NE(PlanStage::ADVANCED, state);
        }
    }


    IndexScan* createIndexScanSimpleRange(BSONObj startKey, BSONObj endKey) {
        IndexCatalog* catalog = _coll->getIndexCatalog();
        std::vector<const IndexDescriptor*> indexes;
        catalog->findIndexesByKeyPattern(
            &_opCtx, BSON("x" << 1), IndexCatalog::InclusionPolicy::kReady, &indexes);
        ASSERT_EQ(indexes.size(), 1U);

        // We are not testing indexing here so use maximal bounds
        IndexScanParams params(&_opCtx, _collPtr, indexes[0]);
        params.bounds.isSimpleRange = true;
        params.bounds.startKey = startKey;
        params.bounds.endKey = endKey;
        params.bounds.boundInclusion = BoundInclusion::kIncludeBothStartAndEndKeys;
        params.direction = 1;

        // This child stage gets owned and freed by the caller.
        MatchExpression* filter = nullptr;
        return new IndexScan(_expCtx.get(), &_collPtr, params, &_ws, filter);
    }

    IndexScan* createIndexScan(BSONObj startKey,
                               BSONObj endKey,
                               bool startInclusive,
                               bool endInclusive,
                               int direction = 1,
                               bool dedup = false,
                               MatchExpression* filter = nullptr) {
        IndexCatalog* catalog = _coll->getIndexCatalog();
        std::vector<const IndexDescriptor*> indexes;
        catalog->findIndexesByKeyPattern(
            &_opCtx, BSON("x" << 1), IndexCatalog::InclusionPolicy::kReady, &indexes);
        ASSERT_EQ(indexes.size(), 1U);

        IndexScanParams params(&_opCtx, _collPtr, indexes[0]);
        params.direction = direction;
        params.shouldDedup = dedup;

        OrderedIntervalList oil("x");
        BSONObjBuilder bob;
        bob.appendAs(startKey.firstElement(), "");
        bob.appendAs(endKey.firstElement(), "");
        oil.intervals.push_back(Interval(bob.obj(), startInclusive, endInclusive));
        params.bounds.fields.push_back(oil);

        return new IndexScan(_expCtx.get(), &_collPtr, params, &_ws, filter);
    }

    static const char* ns() {
        return "unittest.QueryStageIxscan";
    }
    static NamespaceString nss() {
        return NamespaceString::createNamespaceString_forTest(ns());
    }

protected:
    const ServiceContext::UniqueOperationContext _opCtxPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_opCtxPtr;

    Lock::DBLock _dbLock;
    OldClientContext _ctx;
    Collection* _coll;
    CollectionPtr _collPtr;

    WorkingSet _ws;

    boost::intrusive_ptr<ExpressionContext> _expCtx;
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
        ASSERT_BSONOBJ_EQ(stats->keyPattern, BSON("x" << 1));
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
        ASSERT_BSONOBJ_EQ(member->keyData[0].keyData, BSON("" << 5));
        member = getNext(ixscan.get());
        ASSERT_EQ(WorkingSetMember::RID_AND_IDX, member->getState());
        ASSERT_BSONOBJ_EQ(member->keyData[0].keyData, BSON("" << 6));

        // Save state and insert a few indexed docs.
        static_cast<PlanStage*>(ixscan.get())->saveState();
        insert(fromjson("{_id: 4, x: 10}"));
        insert(fromjson("{_id: 5, x: 11}"));
        static_cast<PlanStage*>(ixscan.get())->restoreState(&_collPtr);

        member = getNext(ixscan.get());
        ASSERT_EQ(WorkingSetMember::RID_AND_IDX, member->getState());
        ASSERT_BSONOBJ_EQ(member->keyData[0].keyData, BSON("" << 10));

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
        ASSERT_BSONOBJ_EQ(member->keyData[0].keyData, BSON("" << 6));

        // Save state and insert an indexed doc.
        static_cast<PlanStage*>(ixscan.get())->saveState();
        insert(fromjson("{_id: 4, x: 7}"));
        static_cast<PlanStage*>(ixscan.get())->restoreState(&_collPtr);

        member = getNext(ixscan.get());
        ASSERT_EQ(WorkingSetMember::RID_AND_IDX, member->getState());
        ASSERT_BSONOBJ_EQ(member->keyData[0].keyData, BSON("" << 7));

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
        ASSERT_BSONOBJ_EQ(member->keyData[0].keyData, BSON("" << 6));

        // Save state and insert an indexed doc.
        static_cast<PlanStage*>(ixscan.get())->saveState();
        insert(fromjson("{_id: 4, x: 10}"));
        static_cast<PlanStage*>(ixscan.get())->restoreState(&_collPtr);

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
        ASSERT_BSONOBJ_EQ(member->keyData[0].keyData, BSON("" << 10));
        member = getNext(ixscan.get());
        ASSERT_EQ(WorkingSetMember::RID_AND_IDX, member->getState());
        ASSERT_BSONOBJ_EQ(member->keyData[0].keyData, BSON("" << 8));

        // Save state and insert an indexed doc.
        static_cast<PlanStage*>(ixscan.get())->saveState();
        insert(fromjson("{_id: 4, x: 6}"));
        insert(fromjson("{_id: 5, x: 9}"));
        static_cast<PlanStage*>(ixscan.get())->restoreState(&_collPtr);

        // Ensure that we don't erroneously return {'': 9} or {'':3}.
        member = getNext(ixscan.get());
        ASSERT_EQ(WorkingSetMember::RID_AND_IDX, member->getState());
        ASSERT_BSONOBJ_EQ(member->keyData[0].keyData, BSON("" << 6));

        WorkingSetID id;
        ASSERT_EQ(PlanStage::IS_EOF, ixscan->work(&id));
        ASSERT(ixscan->isEOF());
    }
};

// SERVER-29667
class QueryStageIxscanMultikeyFilter : public IndexScanTest {
public:
    void run() {
        setup();

        // Insert some documents where 'x' is an array.
        insert(fromjson("{_id: 1, x: [2, 1, 3]}"));
        insert(fromjson("{_id: 2, x: [3, -1, 5]}"));
        insert(fromjson("{_id: 3, x: [6, 5, 4]}"));
        insert(fromjson("{_id: 4, x: [2, 4, 6]}"));
        insert(fromjson("{_id: 5, x: [3, 3, 6]}"));
        insert(fromjson("{_id: 6, x: [7, 5, 8]}"));

        // Scan the index across the range [1, 7] for even keys.
        BSONObj bsonObj = BSON("x" << BSON("$mod" << BSON_ARRAY(2 << 0)));
        StatusWithMatchExpression swMatch = MatchExpressionParser::parse(bsonObj, _expCtx.get());
        ASSERT_OK(swMatch.getStatus());
        auto filter = std::move(swMatch.getValue());
        std::unique_ptr<IndexScan> ixscan(
            createIndexScan(BSON("x" << 1), BSON("x" << 7), true, true, 1, true, filter.get()));

        // Expect to get records with ids 1, 4, 3, 5, in that order.
        WorkingSetMember* member = getNext(ixscan.get());
        ASSERT_EQ(WorkingSetMember::RID_AND_IDX, member->getState());
        ASSERT_EQ(member->recordId, RecordId{1});
        ASSERT_BSONOBJ_EQ(member->keyData[0].keyData, BSON("" << 2));
        member = getNext(ixscan.get());
        ASSERT_EQ(WorkingSetMember::RID_AND_IDX, member->getState());
        ASSERT_EQ(member->recordId, RecordId{4});
        ASSERT_BSONOBJ_EQ(member->keyData[0].keyData, BSON("" << 2));
        member = getNext(ixscan.get());
        ASSERT_EQ(WorkingSetMember::RID_AND_IDX, member->getState());
        ASSERT_EQ(member->recordId, RecordId{3});
        ASSERT_BSONOBJ_EQ(member->keyData[0].keyData, BSON("" << 4));
        member = getNext(ixscan.get());
        ASSERT_EQ(WorkingSetMember::RID_AND_IDX, member->getState());
        ASSERT_EQ(member->recordId, RecordId{5});
        ASSERT_BSONOBJ_EQ(member->keyData[0].keyData, BSON("" << 6));
        assertNoNextResult(ixscan.get());

        // Verify that 15 values were tested and 5 duplicates were dropped.
        const IndexScanStats* stats =
            static_cast<const IndexScanStats*>(ixscan->getSpecificStats());
        ASSERT(stats);
        ASSERT_TRUE(stats->isMultiKey);
        ASSERT_EQ(stats->dupsDropped, 5);
        ASSERT_EQ(stats->dupsTested, 15);
    }
};

class All : public unittest::OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("query_stage_ixscan") {}

    void setupTests() override {
        add<QueryStageIxscanInitializeStats>();
        add<QueryStageIxscanInsertDuringSave>();
        add<QueryStageIxscanInsertDuringSaveExclusive>();
        add<QueryStageIxscanInsertDuringSaveExclusive2>();
        add<QueryStageIxscanInsertDuringSaveReverse>();
        add<QueryStageIxscanMultikeyFilter>();
    }
};

unittest::OldStyleSuiteInitializer<All> aueryStageIxscanAll;

}  // namespace QueryStageIxscan
}  // namespace mongo
