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

#include <memory>

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/exec/count.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/query/count_command_gen.h"
#include "mongo/dbtests/dbtests.h"

namespace QueryStageCount {

using std::unique_ptr;
using std::vector;

const int kDocuments = 100;
const int kInterjections = kDocuments;
const NamespaceString kTestNss = NamespaceString("db.dummy");

class CountStageTest {
public:
    CountStageTest()
        : _dbLock(&_opCtx, nss().dbName(), MODE_X),
          _ctx(&_opCtx, ns()),
          _expCtx(make_intrusive<ExpressionContext>(&_opCtx, nullptr, kTestNss)),
          _coll(nullptr) {}

    virtual ~CountStageTest() {}

    virtual void interject(CountStage&, int) {}

    virtual void setup() {
        WriteUnitOfWork wunit(&_opCtx);

        _ctx.db()->dropCollection(&_opCtx, nss()).transitional_ignore();
        auto coll = _ctx.db()->createCollection(&_opCtx, nss());

        coll->getIndexCatalog()
            ->createIndexOnEmptyCollection(&_opCtx,
                                           coll,
                                           BSON("key" << BSON("x" << 1) << "name"
                                                      << "x_1"
                                                      << "v" << 1))
            .status_with_transitional_ignore();
        _coll = coll;

        for (int i = 0; i < kDocuments; i++) {
            insert(BSON(GENOID << "x" << i));
        }

        wunit.commit();
    }

    void getRecordIds() {
        _recordIds.clear();
        WorkingSet ws;

        CollectionScanParams params;
        params.direction = CollectionScanParams::FORWARD;
        params.tailable = false;

        unique_ptr<CollectionScan> scan(
            new CollectionScan(_expCtx.get(), _coll, params, &ws, nullptr));
        while (!scan->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState state = scan->work(&id);
            if (PlanStage::ADVANCED == state) {
                WorkingSetMember* member = ws.get(id);
                verify(member->hasRecordId());
                _recordIds.push_back(member->recordId);
            }
        }
    }

    void insert(const BSONObj& doc) {
        WriteUnitOfWork wunit(&_opCtx);
        OpDebug* const nullOpDebug = nullptr;
        _coll->insertDocument(&_opCtx, InsertStatement(doc), nullOpDebug).transitional_ignore();
        wunit.commit();
    }

    void remove(const RecordId& recordId) {
        WriteUnitOfWork wunit(&_opCtx);
        OpDebug* const nullOpDebug = nullptr;
        _coll->deleteDocument(&_opCtx, kUninitializedStmtId, recordId, nullOpDebug);
        wunit.commit();
    }

    void update(const RecordId& oldrecordId, const BSONObj& newDoc) {
        WriteUnitOfWork wunit(&_opCtx);
        BSONObj oldDoc = _coll->getRecordStore()->dataFor(&_opCtx, oldrecordId).releaseToBson();
        CollectionUpdateArgs args;
        _coll->updateDocument(&_opCtx,
                              oldrecordId,
                              Snapshotted<BSONObj>(_opCtx.recoveryUnit()->getSnapshotId(), oldDoc),
                              newDoc,
                              true,
                              nullptr,
                              &args);
        wunit.commit();
    }

    // testcount is a wrapper around runCount that
    //  - sets up a countStage
    //  - runs it
    //  - asserts count is not trivial
    //  - asserts nCounted is equal to expected_n
    //  - asserts nSkipped is correct
    void testCount(const CountCommandRequest& request,
                   int expected_n = kDocuments,
                   bool indexed = false) {
        setup();
        getRecordIds();

        unique_ptr<WorkingSet> ws(new WorkingSet);

        StatusWithMatchExpression statusWithMatcher =
            MatchExpressionParser::parse(request.getQuery(), _expCtx);
        ASSERT(statusWithMatcher.isOK());
        unique_ptr<MatchExpression> expression = std::move(statusWithMatcher.getValue());

        PlanStage* scan;
        if (indexed) {
            scan = createIndexScan(expression.get(), ws.get());
        } else {
            scan = createCollScan(expression.get(), ws.get());
        }

        CountStage countStage(_expCtx.get(),
                              _coll,
                              request.getLimit().value_or(0),
                              request.getSkip().value_or(0),
                              ws.get(),
                              scan);

        const CountStats* stats = runCount(countStage);

        ASSERT_EQUALS(stats->nCounted, expected_n);
        ASSERT_EQUALS(stats->nSkipped, request.getSkip().value_or(0));
    }

    // Performs a test using a count stage whereby each unit of work is interjected
    // in some way by the invocation of interject().
    const CountStats* runCount(CountStage& countStage) {
        int interjection = 0;
        WorkingSetID wsid;

        while (!countStage.isEOF()) {
            countStage.work(&wsid);
            // Prepare for yield.
            countStage.saveState();

            // Interject in some way kInterjection times.
            if (interjection < kInterjections) {
                interject(countStage, interjection++);
            }

            // Resume from yield.
            countStage.restoreState(&_coll);
        }

        return static_cast<const CountStats*>(countStage.getSpecificStats());
    }

    IndexScan* createIndexScan(MatchExpression* expr, WorkingSet* ws) {
        const IndexCatalog* catalog = _coll->getIndexCatalog();
        std::vector<const IndexDescriptor*> indexes;
        catalog->findIndexesByKeyPattern(
            &_opCtx, BSON("x" << 1), IndexCatalog::InclusionPolicy::kReady, &indexes);
        ASSERT_EQ(indexes.size(), 1U);
        auto descriptor = indexes[0];

        // We are not testing indexing here so use maximal bounds
        IndexScanParams params(&_opCtx, _coll, descriptor);
        params.bounds.isSimpleRange = true;
        params.bounds.startKey = BSON("" << 0);
        params.bounds.endKey = BSON("" << kDocuments + 1);
        params.bounds.boundInclusion = BoundInclusion::kIncludeBothStartAndEndKeys;
        params.direction = 1;

        // This child stage gets owned and freed by its parent CountStage
        return new IndexScan(_expCtx.get(), _coll, params, ws, expr);
    }

    CollectionScan* createCollScan(MatchExpression* expr, WorkingSet* ws) {
        CollectionScanParams params;

        // This child stage gets owned and freed by its parent CountStage
        return new CollectionScan(_expCtx.get(), _coll, params, ws, expr);
    }

    static const char* ns() {
        return "unittest.QueryStageCount";
    }

    static NamespaceString nss() {
        return NamespaceString(ns());
    }

protected:
    vector<RecordId> _recordIds;
    const ServiceContext::UniqueOperationContext _opCtxPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_opCtxPtr;
    Lock::DBLock _dbLock;
    OldClientContext _ctx;
    boost::intrusive_ptr<ExpressionContext> _expCtx;
    CollectionPtr _coll;
};

class QueryStageCountNoChangeDuringYield : public CountStageTest {
public:
    void run() {
        CountCommandRequest request((NamespaceString(ns())));
        request.setQuery(BSON("x" << LT << kDocuments / 2));

        testCount(request, kDocuments / 2);
        testCount(request, kDocuments / 2, true);
    }
};

class QueryStageCountYieldWithSkip : public CountStageTest {
public:
    void run() {
        CountCommandRequest request((NamespaceString(ns())));
        request.setQuery(BSON("x" << GTE << 0));
        request.setSkip(2);

        testCount(request, kDocuments - 2);
        testCount(request, kDocuments - 2, true);
    }
};

class QueryStageCountYieldWithLimit : public CountStageTest {
public:
    void run() {
        CountCommandRequest request((NamespaceString(ns())));
        request.setQuery(BSON("x" << GTE << 0));
        request.setSkip(0);
        request.setLimit(2);

        testCount(request, 2);
        testCount(request, 2, true);
    }
};


class QueryStageCountInsertDuringYield : public CountStageTest {
public:
    void run() {
        CountCommandRequest request((NamespaceString(ns())));
        request.setQuery(BSON("x" << 1));

        testCount(request, kInterjections + 1);
        testCount(request, kInterjections + 1, true);
    }

    // This is called 100 times as we scan the collection
    void interject(CountStage&, int) {
        insert(BSON(GENOID << "x" << 1));
    }
};

class QueryStageCountDeleteDuringYield : public CountStageTest {
public:
    void run() {
        // expected count would be 99 but we delete the second record
        // after doing the first unit of work
        CountCommandRequest request((NamespaceString(ns())));
        request.setQuery(BSON("x" << GTE << 1));

        testCount(request, kDocuments - 2);
        testCount(request, kDocuments - 2, true);
    }

    // At the point which this is called we are in between counting the first + second record
    void interject(CountStage& count_stage, int interjection) {
        if (interjection == 0) {
            // At this point, our first interjection, we've counted _recordIds[0]
            // and are about to count _recordIds[1]
            WriteUnitOfWork wunit(&_opCtx);
            remove(_recordIds[interjection]);

            remove(_recordIds[interjection + 1]);
            wunit.commit();
        }
    }
};

class QueryStageCountUpdateDuringYield : public CountStageTest {
public:
    void run() {
        // expected count would be kDocuments-2 but we update the first and second records
        // after doing the first unit of work so they wind up getting counted later on
        CountCommandRequest request((NamespaceString(ns())));
        request.setQuery(BSON("x" << GTE << 2));

        testCount(request, kDocuments);
        testCount(request, kDocuments, true);
    }

    // At the point which this is called we are in between the first and second record
    void interject(CountStage& count_stage, int interjection) {
        if (interjection == 0) {
            OID id1 = _coll->docFor(&_opCtx, _recordIds[0]).value().getField("_id").OID();
            update(_recordIds[0], BSON("_id" << id1 << "x" << 100));

            OID id2 = _coll->docFor(&_opCtx, _recordIds[1]).value().getField("_id").OID();
            update(_recordIds[1], BSON("_id" << id2 << "x" << 100));
        }
    }
};

class QueryStageCountMultiKeyDuringYield : public CountStageTest {
public:
    void run() {
        CountCommandRequest request((NamespaceString(ns())));
        request.setQuery(BSON("x" << 1));
        testCount(request, kDocuments + 1, true);  // only applies to indexed case
    }

    void interject(CountStage&, int) {
        // Should cause index to be converted to multikey
        insert(BSON(GENOID << "x" << BSON_ARRAY(1 << 2)));
    }
};

class All : public OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("query_stage_count") {}

    void setupTests() {
        add<QueryStageCountNoChangeDuringYield>();
        add<QueryStageCountYieldWithSkip>();
        add<QueryStageCountYieldWithLimit>();
        add<QueryStageCountInsertDuringYield>();
        add<QueryStageCountDeleteDuringYield>();
        add<QueryStageCountUpdateDuringYield>();
        add<QueryStageCountMultiKeyDuringYield>();
    }
};

OldStyleSuiteInitializer<All> queryStageCountAll;

}  // namespace QueryStageCount
