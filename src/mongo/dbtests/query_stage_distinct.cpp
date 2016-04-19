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
#include "mongo/db/exec/distinct_scan.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/json.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/dbtests/dbtests.h"

/**
 * This file tests db/exec/distinct.cpp
 */

namespace QueryStageDistinct {

class DistinctBase {
public:
    DistinctBase() : _client(&_txn) {}

    virtual ~DistinctBase() {
        _client.dropCollection(ns());
    }

    void addIndex(const BSONObj& obj) {
        ASSERT_OK(dbtests::createIndex(&_txn, ns(), obj));
    }

    void insert(const BSONObj& obj) {
        _client.insert(ns(), obj);
    }

    /**
     * Returns the projected value from the working set that would
     * be returned in the 'values' field of the distinct command result.
     * Limited to NumberInt BSON types because this is the only
     * BSON type used in this suite of tests.
     */
    static int getIntFieldDotted(const WorkingSet& ws,
                                 WorkingSetID wsid,
                                 const std::string& field) {
        // For some reason (at least under OS X clang), we cannot refer to INVALID_ID
        // inside the test assertion macro.
        WorkingSetID invalid = WorkingSet::INVALID_ID;
        ASSERT_NOT_EQUALS(invalid, wsid);

        WorkingSetMember* member = ws.get(wsid);

        // Distinct hack execution is always covered.
        // Key value is retrieved from working set key data
        // instead of RecordId.
        ASSERT_FALSE(member->hasObj());
        BSONElement keyElt;
        ASSERT_TRUE(member->getFieldDotted(field, &keyElt));
        ASSERT_TRUE(keyElt.isNumber());

        return keyElt.numberInt();
    }

    static const char* ns() {
        return "unittests.QueryStageDistinct";
    }

protected:
    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _txn = *_txnPtr;

private:
    DBDirectClient _client;
};


// Tests distinct with single key indices.
class QueryStageDistinctBasic : public DistinctBase {
public:
    virtual ~QueryStageDistinctBasic() {}

    void run() {
        // Insert a ton of documents with a: 1
        for (size_t i = 0; i < 1000; ++i) {
            insert(BSON("a" << 1));
        }

        // Insert a ton of other documents with a: 2
        for (size_t i = 0; i < 1000; ++i) {
            insert(BSON("a" << 2));
        }

        // Make an index on a:1
        addIndex(BSON("a" << 1));

        AutoGetCollectionForRead ctx(&_txn, ns());
        Collection* coll = ctx.getCollection();

        // Set up the distinct stage.
        DistinctParams params;
        params.descriptor = coll->getIndexCatalog()->findIndexByKeyPattern(&_txn, BSON("a" << 1));
        verify(params.descriptor);
        params.direction = 1;
        // Distinct-ing over the 0-th field of the keypattern.
        params.fieldNo = 0;
        // We'll look at all values in the bounds.
        params.bounds.isSimpleRange = false;
        OrderedIntervalList oil("a");
        oil.intervals.push_back(IndexBoundsBuilder::allValues());
        params.bounds.fields.push_back(oil);

        WorkingSet ws;
        DistinctScan distinct(&_txn, params, &ws);

        WorkingSetID wsid;
        // Get our first result.
        int firstResultWorks = 0;
        while (PlanStage::ADVANCED != distinct.work(&wsid)) {
            ++firstResultWorks;
        }
        // 5 is a bogus number.  There's some amount of setup done by the first few calls but
        // we should return the first result relatively promptly.
        ASSERT_LESS_THAN(firstResultWorks, 5);
        ASSERT_EQUALS(1, getIntFieldDotted(ws, wsid, "a"));

        // Getting our second result should be very quick as we just skip
        // over the first result.
        int secondResultWorks = 0;
        while (PlanStage::ADVANCED != distinct.work(&wsid)) {
            ++secondResultWorks;
        }
        ASSERT_EQUALS(2, getIntFieldDotted(ws, wsid, "a"));
        // This is 0 because we don't have to loop for several values; we just skip over
        // all the 'a' values.
        ASSERT_EQUALS(0, secondResultWorks);

        ASSERT_EQUALS(PlanStage::IS_EOF, distinct.work(&wsid));
    }
};

// Tests distinct with multikey indices.
class QueryStageDistinctMultiKey : public DistinctBase {
public:
    virtual ~QueryStageDistinctMultiKey() {}

    void run() {
        // Insert a ton of documents with a: [1, 2, 3]
        for (size_t i = 0; i < 1000; ++i) {
            insert(BSON("a" << BSON_ARRAY(1 << 2 << 3)));
        }

        // Insert a ton of other documents with a: [4, 5, 6]
        for (size_t i = 0; i < 1000; ++i) {
            insert(BSON("a" << BSON_ARRAY(4 << 5 << 6)));
        }

        // Make an index on a:1
        addIndex(BSON("a" << 1));

        AutoGetCollectionForRead ctx(&_txn, ns());
        Collection* coll = ctx.getCollection();

        // Set up the distinct stage.
        DistinctParams params;
        params.descriptor = coll->getIndexCatalog()->findIndexByKeyPattern(&_txn, BSON("a" << 1));
        ASSERT_TRUE(params.descriptor->isMultikey(&_txn));

        verify(params.descriptor);
        params.direction = 1;
        // Distinct-ing over the 0-th field of the keypattern.
        params.fieldNo = 0;
        // We'll look at all values in the bounds.
        params.bounds.isSimpleRange = false;
        OrderedIntervalList oil("a");
        oil.intervals.push_back(IndexBoundsBuilder::allValues());
        params.bounds.fields.push_back(oil);

        WorkingSet ws;
        DistinctScan distinct(&_txn, params, &ws);

        // We should see each number in the range [1, 6] exactly once.
        std::set<int> seen;

        WorkingSetID wsid;
        PlanStage::StageState state;
        while (PlanStage::IS_EOF != (state = distinct.work(&wsid))) {
            if (PlanStage::ADVANCED == state) {
                // Check int value.
                int currentNumber = getIntFieldDotted(ws, wsid, "a");
                ASSERT_GREATER_THAN_OR_EQUALS(currentNumber, 1);
                ASSERT_LESS_THAN_OR_EQUALS(currentNumber, 6);

                // Should see this number only once.
                ASSERT_TRUE(seen.find(currentNumber) == seen.end());
                seen.insert(currentNumber);
            }
        }

        ASSERT_EQUALS(6U, seen.size());
    }
};

// XXX: add a test case with bounds where skipping to the next key gets us a result that's not
// valid w.r.t. our query.

class All : public Suite {
public:
    All() : Suite("query_stage_distinct") {}

    void setupTests() {
        add<QueryStageDistinctBasic>();
        add<QueryStageDistinctMultiKey>();
    }
};

SuiteInstance<All> queryStageDistinctAll;

}  // namespace QueryStageDistinct
