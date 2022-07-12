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

#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/count_scan.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/util/fail_point.h"

namespace QueryStageCountScan {

using std::shared_ptr;

class CountBase {
public:
    CountBase() : _client(&_opCtx) {}

    virtual ~CountBase() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns().ns());
        _client.dropCollection(ns().ns());
    }

    void addIndex(const BSONObj& obj) {
        ASSERT_OK(dbtests::createIndex(&_opCtx, ns().ns(), obj));
    }

    void insert(const BSONObj& obj) {
        _client.insert(ns().ns(), obj);
    }

    void remove(const BSONObj& obj) {
        _client.remove(ns().ns(), obj);
    }

    /*
     * Given a CountScan PlanStage object count, call work() on
     * count until we reach IS_EOF.  Tally up how many objects
     * we've counted and return the count.
     */
    int runCount(CountScan* count) {
        int countWorks = 0;
        WorkingSetID wsid;

        PlanStage::StageState countState = count->work(&wsid);

        while (PlanStage::IS_EOF != countState) {
            if (PlanStage::ADVANCED == countState)
                countWorks++;
            countState = count->work(&wsid);
        }
        return countWorks;
    }

    CollectionPtr getCollection() {
        return CollectionCatalog::get(&_opCtx)->lookupCollectionByNamespace(&_opCtx, ns());
    }

    const IndexDescriptor* getIndex(Database* db, const BSONObj& obj) {
        std::vector<const IndexDescriptor*> indexes;
        getCollection()->getIndexCatalog()->findIndexesByKeyPattern(
            &_opCtx, obj, IndexCatalog::InclusionPolicy::kReady, &indexes);
        return indexes.empty() ? nullptr : indexes[0];
    }

    CountScanParams makeCountScanParams(OperationContext* opCtx,
                                        const CollectionPtr& collection,
                                        const IndexDescriptor* descriptor) {
        return {opCtx, collection, descriptor};
    }

    static NamespaceString ns() {
        return {"unittests", "QueryStageCountScanScan"};
    }

protected:
    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_txnPtr;

    boost::intrusive_ptr<ExpressionContext> _expCtx =
        make_intrusive<ExpressionContext>(&_opCtx, nullptr, ns());

private:
    DBDirectClient _client;
};


//
// Check that dups are properly identified
//
class QueryStageCountScanDups : public CountBase {
public:
    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns().ns());

        // Insert some docs
        insert(BSON("a" << BSON_ARRAY(5 << 7)));
        insert(BSON("a" << BSON_ARRAY(6 << 8)));

        // Add an index on a:1
        addIndex(BSON("a" << 1));

        const CollectionPtr coll = ctx.getCollection();

        // Set up the count stage
        auto params = makeCountScanParams(&_opCtx, coll, getIndex(ctx.db(), BSON("a" << 1)));
        params.startKey = BSON("a" << 1);
        params.startKeyInclusive = true;
        params.endKey = BSON("a" << 10);
        params.endKeyInclusive = true;

        WorkingSet ws;
        CountScan count(_expCtx.get(), coll, params, &ws);

        int numCounted = runCount(&count);
        ASSERT_EQUALS(2, numCounted);
    }
};

//
// Check that expected results are returned with inclusive bounds
//
class QueryStageCountScanInclusiveBounds : public CountBase {
public:
    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns().ns());

        // Insert some docs
        for (int i = 0; i < 10; ++i) {
            insert(BSON("a" << i));
        }

        // Add an index
        addIndex(BSON("a" << 1));

        const CollectionPtr coll = ctx.getCollection();

        // Set up the count stage
        auto params = makeCountScanParams(&_opCtx, coll, getIndex(ctx.db(), BSON("a" << 1)));
        params.startKey = BSON("" << 3);
        params.startKeyInclusive = true;
        params.endKey = BSON("" << 7);
        params.endKeyInclusive = true;

        WorkingSet ws;
        CountScan count(_expCtx.get(), coll, params, &ws);

        int numCounted = runCount(&count);
        ASSERT_EQUALS(5, numCounted);
    }
};

//
// Check that expected results are returned with exclusive bounds
//
class QueryStageCountScanExclusiveBounds : public CountBase {
public:
    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns().ns());

        // Insert some docs
        for (int i = 0; i < 10; ++i) {
            insert(BSON("a" << i));
        }

        // Add an index
        addIndex(BSON("a" << 1));

        const CollectionPtr coll = ctx.getCollection();

        // Set up the count stage
        auto params = makeCountScanParams(&_opCtx, coll, getIndex(ctx.db(), BSON("a" << 1)));
        params.startKey = BSON("" << 3);
        params.startKeyInclusive = false;
        params.endKey = BSON("" << 7);
        params.endKeyInclusive = false;

        WorkingSet ws;
        CountScan count(_expCtx.get(), coll, params, &ws);

        int numCounted = runCount(&count);
        ASSERT_EQUALS(3, numCounted);
    }
};

//
// Check that cursor returns no results if all docs are below lower bound
//
class QueryStageCountScanLowerBound : public CountBase {
public:
    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns().ns());

        // Insert doc, add index
        insert(BSON("a" << 2));
        addIndex(BSON("a" << 1));

        const CollectionPtr coll = ctx.getCollection();

        // Set up count, and run
        auto params = makeCountScanParams(&_opCtx, coll, getIndex(ctx.db(), BSON("a" << 1)));
        params.startKey = BSON("" << 2);
        params.startKeyInclusive = false;
        params.endKey = BSON("" << 3);
        params.endKeyInclusive = false;

        WorkingSet ws;
        CountScan count(_expCtx.get(), coll, params, &ws);

        int numCounted = runCount(&count);
        ASSERT_EQUALS(0, numCounted);
    }
};

//
// Check that cursor returns no results if there are no docs within interval
//
class QueryStageCountScanNothingInInterval : public CountBase {
public:
    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns().ns());

        // Insert documents, add index
        insert(BSON("a" << 2));
        insert(BSON("a" << 3));
        addIndex(BSON("a" << 1));

        const CollectionPtr coll = ctx.getCollection();

        // Set up count, and run
        auto params = makeCountScanParams(&_opCtx, coll, getIndex(ctx.db(), BSON("a" << 1)));
        params.startKey = BSON("" << 2);
        params.startKeyInclusive = false;
        params.endKey = BSON("" << 3);
        params.endKeyInclusive = false;

        WorkingSet ws;
        CountScan count(_expCtx.get(), coll, params, &ws);

        int numCounted = runCount(&count);
        ASSERT_EQUALS(0, numCounted);
    }
};

//
// Check that cursor returns no results if there are no docs within interval
// and the first key located during initialization is above upper bound
//
class QueryStageCountScanNothingInIntervalFirstMatchTooHigh : public CountBase {
public:
    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns().ns());

        // Insert some documents, add index
        insert(BSON("a" << 2));
        insert(BSON("a" << 4));
        addIndex(BSON("a" << 1));

        const CollectionPtr coll = ctx.getCollection();

        // Set up count, and run
        auto params = makeCountScanParams(&_opCtx, coll, getIndex(ctx.db(), BSON("a" << 1)));
        params.startKey = BSON("" << 2);
        params.startKeyInclusive = false;
        params.endKey = BSON("" << 3);
        params.endKeyInclusive = true;

        WorkingSet ws;
        CountScan count(_expCtx.get(), coll, params, &ws);

        int numCounted = runCount(&count);
        ASSERT_EQUALS(0, numCounted);
    }
};

//
// Check that cursor recovers its position properly if there is no change
// during a yield
//
class QueryStageCountScanNoChangeDuringYield : public CountBase {
public:
    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns().ns());

        // Insert documents, add index
        for (int i = 0; i < 10; ++i) {
            insert(BSON("a" << i));
        }
        addIndex(BSON("a" << 1));

        const CollectionPtr coll = ctx.getCollection();

        // Set up count stage
        auto params = makeCountScanParams(&_opCtx, coll, getIndex(ctx.db(), BSON("a" << 1)));
        params.startKey = BSON("" << 2);
        params.startKeyInclusive = false;
        params.endKey = BSON("" << 6);
        params.endKeyInclusive = true;

        WorkingSet ws;
        CountScan count(_expCtx.get(), coll, params, &ws);
        WorkingSetID wsid;

        int numCounted = 0;
        PlanStage::StageState countState;

        // Begin running the count
        while (numCounted < 2) {
            countState = count.work(&wsid);
            if (PlanStage::ADVANCED == countState)
                numCounted++;
        }

        // Prepare the cursor to yield
        static_cast<PlanStage*>(&count)->saveState();

        // Recover from yield
        auto c = getCollection();

        static_cast<PlanStage*>(&count)->restoreState(&coll);

        // finish counting
        while (PlanStage::IS_EOF != countState) {
            countState = count.work(&wsid);
            if (PlanStage::ADVANCED == countState)
                numCounted++;
        }
        ASSERT_EQUALS(4, numCounted);
    }
};

//
// Check that cursor recovers its position properly if its current location
// is deleted during a yield
//
class QueryStageCountScanDeleteDuringYield : public CountBase {
public:
    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns().ns());

        // Insert documents, add index
        for (int i = 0; i < 10; ++i) {
            insert(BSON("a" << i));
        }
        addIndex(BSON("a" << 1));

        const CollectionPtr coll = ctx.getCollection();

        // Set up count stage
        auto params = makeCountScanParams(&_opCtx, coll, getIndex(ctx.db(), BSON("a" << 1)));
        params.startKey = BSON("" << 2);
        params.startKeyInclusive = false;
        params.endKey = BSON("" << 6);
        params.endKeyInclusive = true;

        WorkingSet ws;
        CountScan count(_expCtx.get(), coll, params, &ws);
        WorkingSetID wsid;

        int numCounted = 0;
        PlanStage::StageState countState;

        // Begin running the count
        while (numCounted < 2) {
            countState = count.work(&wsid);
            if (PlanStage::ADVANCED == countState)
                numCounted++;
        }

        // Prepare the cursor to yield
        static_cast<PlanStage*>(&count)->saveState();

        // Recover from yield
        auto c = getCollection();

        // Remove remaining objects
        remove(BSON("a" << GTE << 5));

        static_cast<PlanStage*>(&count)->restoreState(&coll);

        // finish counting
        while (PlanStage::IS_EOF != countState) {
            countState = count.work(&wsid);
            if (PlanStage::ADVANCED == countState)
                numCounted++;
        }
        ASSERT_EQUALS(2, numCounted);
    }
};

//
// Check that cursor relocates its end location properly if end location
// changes during a yield
//
class QueryStageCountScanInsertNewDocsDuringYield : public CountBase {
public:
    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns().ns());

        // Insert documents, add index
        for (int i = 0; i < 10; ++i) {
            insert(BSON("a" << i));
        }
        addIndex(BSON("a" << 1));

        const CollectionPtr coll = ctx.getCollection();

        // Set up count stage
        auto params = makeCountScanParams(&_opCtx, coll, getIndex(ctx.db(), BSON("a" << 1)));
        params.startKey = BSON("" << 2);
        params.startKeyInclusive = false;
        params.endKey = BSON("" << 6);
        params.endKeyInclusive = true;

        WorkingSet ws;
        CountScan count(_expCtx.get(), coll, params, &ws);
        WorkingSetID wsid;

        int numCounted = 0;
        PlanStage::StageState countState;

        // Begin running the count
        while (numCounted < 2) {
            countState = count.work(&wsid);
            if (PlanStage::ADVANCED == countState)
                numCounted++;
        }

        // Prepare the cursor to yield
        static_cast<PlanStage*>(&count)->saveState();

        // Insert one document before the end
        insert(BSON("a" << 5.5));

        // Insert one document after the end
        insert(BSON("a" << 6.5));

        // Recover from yield
        auto c = getCollection();

        static_cast<PlanStage*>(&count)->restoreState(&coll);

        // finish counting
        while (PlanStage::IS_EOF != countState) {
            countState = count.work(&wsid);
            if (PlanStage::ADVANCED == countState)
                numCounted++;
        }
        ASSERT_EQUALS(5, numCounted);
    }
};

//
// Unused keys are not returned during iteration
//
class QueryStageCountScanUnusedKeys : public CountBase {
public:
    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns().ns());

        // Insert docs, add index
        for (int i = 0; i < 10; ++i) {
            insert(BSON("a" << 1 << "b" << i));
        }
        addIndex(BSON("a" << 1));

        // Mark several keys as 'unused'
        remove(BSON("a" << 1 << "b" << 0));
        remove(BSON("a" << 1 << "b" << 3));
        remove(BSON("a" << 1 << "b" << 4));

        const CollectionPtr coll = ctx.getCollection();

        // Ensure that count does not include unused keys
        auto params = makeCountScanParams(&_opCtx, coll, getIndex(ctx.db(), BSON("a" << 1)));
        params.startKey = BSON("" << 1);
        params.startKeyInclusive = true;
        params.endKey = BSON("" << 1);
        params.endKeyInclusive = true;

        WorkingSet ws;
        CountScan count(_expCtx.get(), coll, params, &ws);

        int numCounted = runCount(&count);
        ASSERT_EQUALS(7, numCounted);
    }
};

//
// Iteration is properly terminated when the end location is an unused key
//
class QueryStageCountScanUnusedEndKey : public CountBase {
public:
    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns().ns());

        // Insert docs, add index
        for (int i = 0; i < 10; ++i) {
            insert(BSON("a" << 1 << "b" << i));
        }
        addIndex(BSON("a" << 1));

        // Mark key at end position as 'unused' by deleting
        remove(BSON("a" << 1 << "b" << 9));

        const CollectionPtr coll = ctx.getCollection();

        // Run count and check
        auto params = makeCountScanParams(&_opCtx, coll, getIndex(ctx.db(), BSON("a" << 1)));
        params.startKey = BSON("" << 0);
        params.startKeyInclusive = true;
        params.endKey = BSON("" << 2);
        params.endKeyInclusive = true;  // yes?

        WorkingSet ws;
        CountScan count(_expCtx.get(), coll, params, &ws);

        int numCounted = runCount(&count);
        ASSERT_EQUALS(9, numCounted);
    }
};

//
// Advances past a key that becomes unused during a yield
//
class QueryStageCountScanKeyBecomesUnusedDuringYield : public CountBase {
public:
    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns().ns());

        // Insert documents, add index
        for (int i = 0; i < 10; ++i) {
            insert(BSON("a" << 1 << "b" << i));
        }
        addIndex(BSON("a" << 1));

        const CollectionPtr coll = ctx.getCollection();

        // Set up count stage
        auto params = makeCountScanParams(&_opCtx, coll, getIndex(ctx.db(), BSON("a" << 1)));
        params.startKey = BSON("" << 1);
        params.startKeyInclusive = true;
        params.endKey = BSON("" << 1);
        params.endKeyInclusive = true;

        WorkingSet ws;
        CountScan count(_expCtx.get(), coll, params, &ws);
        WorkingSetID wsid;

        int numCounted = 0;
        PlanStage::StageState countState;

        // Begin running the count
        while (numCounted < 2) {
            countState = count.work(&wsid);
            if (PlanStage::ADVANCED == countState)
                numCounted++;
        }

        // Prepare the cursor to yield
        static_cast<PlanStage*>(&count)->saveState();

        // Mark the key at position 5 as 'unused'
        remove(BSON("a" << 1 << "b" << 5));

        // Recover from yield
        auto c = getCollection();
        static_cast<PlanStage*>(&count)->restoreState(&coll);

        // finish counting
        while (PlanStage::IS_EOF != countState) {
            countState = count.work(&wsid);
            if (PlanStage::ADVANCED == countState)
                numCounted++;
        }
        ASSERT_EQUALS(8, numCounted);
    }
};

class All : public OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("query_stage_count_scan") {}

    void setupTests() {
        add<QueryStageCountScanDups>();
        add<QueryStageCountScanInclusiveBounds>();
        add<QueryStageCountScanExclusiveBounds>();
        add<QueryStageCountScanLowerBound>();
        add<QueryStageCountScanNothingInInterval>();
        add<QueryStageCountScanNothingInIntervalFirstMatchTooHigh>();
        add<QueryStageCountScanNoChangeDuringYield>();
        add<QueryStageCountScanDeleteDuringYield>();
        add<QueryStageCountScanInsertNewDocsDuringYield>();
        add<QueryStageCountScanUnusedKeys>();
    }
};

OldStyleSuiteInitializer<All> queryStageCountScanAll;

}  // namespace QueryStageCountScan
