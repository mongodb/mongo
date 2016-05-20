/**
 *    Copyright (C) 2013 mongoDB Inc.
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

/**
 * This file contains tests for mongo/db/exec/sort.cpp
 */

#include "mongo/db/exec/sort.h"

#include "mongo/db/exec/queued_data_stage.h"
#include "mongo/db/json.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/query/collation/collator_factory_mock.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

using namespace mongo;

namespace {

class SortStageTest : public unittest::Test {
public:
    SortStageTest() {
        _service = stdx::make_unique<ServiceContextNoop>();
        _service.get()->setFastClockSource(stdx::make_unique<ClockSourceMock>());
        _client = _service.get()->makeClient("test");
        _opCtxNoop.reset(new OperationContextNoop(_client.get(), 0));
        _opCtx = _opCtxNoop.get();
        CollatorFactoryInterface::set(_service.get(), stdx::make_unique<CollatorFactoryMock>());
    }

    OperationContext* getOpCtx() {
        return _opCtx;
    }

    /**
     * Test function to verify sort stage.
     * SortStageParams will be initialized using patternStr, collator, queryStr and limit.
     * inputStr represents the input data set in a BSONObj.
     *     {input: [doc1, doc2, doc3, ...]}
     * expectedStr represents the expected sorted data set.
     *     {output: [docA, docB, docC, ...]}
     */
    void testWork(const char* patternStr,
                  CollatorInterface* collator,
                  const char* queryStr,
                  int limit,
                  const char* inputStr,
                  const char* expectedStr) {
        // WorkingSet is not owned by stages
        // so it's fine to declare
        WorkingSet ws;

        // QueuedDataStage will be owned by SortStage.
        auto queuedDataStage = stdx::make_unique<QueuedDataStage>(getOpCtx(), &ws);
        BSONObj inputObj = fromjson(inputStr);
        BSONElement inputElt = inputObj.getField("input");
        ASSERT(inputElt.isABSONObj());
        BSONObjIterator inputIt(inputElt.embeddedObject());
        while (inputIt.more()) {
            BSONElement elt = inputIt.next();
            ASSERT(elt.isABSONObj());
            BSONObj obj = elt.embeddedObject().getOwned();

            // Insert obj from input array into working set.
            WorkingSetID id = ws.allocate();
            WorkingSetMember* wsm = ws.get(id);
            wsm->obj = Snapshotted<BSONObj>(SnapshotId(), obj);
            wsm->transitionToOwnedObj();
            queuedDataStage->pushBack(id);
        }

        // Initialize SortStageParams
        // Setting limit to 0 means no limit
        SortStageParams params;
        params.pattern = fromjson(patternStr);
        params.limit = limit;

        auto sortKeyGen = stdx::make_unique<SortKeyGeneratorStage>(getOpCtx(),
                                                                   queuedDataStage.release(),
                                                                   &ws,
                                                                   params.pattern,
                                                                   fromjson(queryStr),
                                                                   collator);

        SortStage sort(getOpCtx(), params, &ws, sortKeyGen.release());

        WorkingSetID id = WorkingSet::INVALID_ID;
        PlanStage::StageState state = PlanStage::NEED_TIME;

        // Keep working sort stage until data is available.
        while (state == PlanStage::NEED_TIME) {
            state = sort.work(&id);
        }

        // QueuedDataStage's state should be EOF when sort is ready to advance.
        ASSERT_TRUE(sort.child()->child()->isEOF());

        // While there's data to be retrieved, state should be equal to ADVANCED.
        // Insert documents into BSON document in this format:
        //     {output: [docA, docB, docC, ...]}
        BSONObjBuilder bob;
        BSONArrayBuilder arr(bob.subarrayStart("output"));
        while (state == PlanStage::ADVANCED) {
            WorkingSetMember* member = ws.get(id);
            const BSONObj& obj = member->obj.value();
            arr.append(obj);
            state = sort.work(&id);
        }
        arr.doneFast();
        BSONObj outputObj = bob.obj();

        // Sort stage should be EOF after data is retrieved.
        ASSERT_EQUALS(state, PlanStage::IS_EOF);
        ASSERT_TRUE(sort.isEOF());

        // Finally, we get to compare the sorted results against what we expect.
        BSONObj expectedObj = fromjson(expectedStr);
        if (outputObj != expectedObj) {
            mongoutils::str::stream ss;
            // Even though we have the original string representation of the expected output,
            // we invoke BSONObj::toString() to get a format consistent with outputObj.
            ss << "Unexpected sort result with query=" << queryStr << "; pattern=" << patternStr
               << "; limit=" << limit << ":\n"
               << "Expected: " << expectedObj.toString() << "\n"
               << "Actual:   " << outputObj.toString() << "\n";
            FAIL(ss);
        }
    }

private:
    OperationContext* _opCtx;

    std::unique_ptr<ServiceContextNoop> _service;

    // Members of a class are destroyed in reverse order of declaration.
    // The UniqueClient must be destroyed before the ServiceContextNoop is destroyed.
    // The OperationContextNoop must be destroyed before the UniqueClient is destroyed.
    ServiceContext::UniqueClient _client;
    std::unique_ptr<OperationContextNoop> _opCtxNoop;
};

TEST_F(SortStageTest, SortEmptyWorkingSet) {
    WorkingSet ws;

    // QueuedDataStage will be owned by SortStage.
    auto queuedDataStage = stdx::make_unique<QueuedDataStage>(getOpCtx(), &ws);
    auto sortKeyGen = stdx::make_unique<SortKeyGeneratorStage>(
        getOpCtx(), queuedDataStage.release(), &ws, BSONObj(), BSONObj(), nullptr);
    SortStageParams params;
    SortStage sort(getOpCtx(), params, &ws, sortKeyGen.release());

    // Check initial EOF state.
    ASSERT_FALSE(sort.isEOF());

    // First call to work() initializes sort key generator.
    WorkingSetID id = WorkingSet::INVALID_ID;
    PlanStage::StageState state = sort.work(&id);
    ASSERT_EQUALS(state, PlanStage::NEED_TIME);

    // Second call to work() sorts data in vector.
    state = sort.work(&id);
    ASSERT_EQUALS(state, PlanStage::NEED_TIME);

    // Finally we hit EOF.
    state = sort.work(&id);
    ASSERT_EQUALS(state, PlanStage::IS_EOF);

    ASSERT_TRUE(sort.isEOF());
}

//
// Limit values
// The server interprets limit values from the user as follows:
//     0: no limit on query results. This is passed along unchanged to the sort stage.
//     >0: soft limit. Also unchanged in sort stage.
//     <0: hard limit. Absolute value is stored in parsed query and passed to sort stage.
// The sort stage treats both soft and hard limits in the same manner

//
// Sort without limit
// Implementation should keep all items fetched from child.
//

TEST_F(SortStageTest, SortAscending) {
    testWork("{a: 1}",
             nullptr,
             "{}",
             0,
             "{input: [{a: 2}, {a: 1}, {a: 3}]}",
             "{output: [{a: 1}, {a: 2}, {a: 3}]}");
}

TEST_F(SortStageTest, SortDescending) {
    testWork("{a: -1}",
             nullptr,
             "{}",
             0,
             "{input: [{a: 2}, {a: 1}, {a: 3}]}",
             "{output: [{a: 3}, {a: 2}, {a: 1}]}");
}

TEST_F(SortStageTest, SortIrrelevantSortKey) {
    testWork("{b: 1}",
             nullptr,
             "{}",
             0,
             "{input: [{a: 2}, {a: 1}, {a: 3}]}",
             "{output: [{a: 2}, {a: 1}, {a: 3}]}");
}

//
// Sorting with limit > 1
// Implementation should retain top N items
// and discard the rest.
//

TEST_F(SortStageTest, SortAscendingWithLimit) {
    testWork("{a: 1}",
             nullptr,
             "{}",
             2,
             "{input: [{a: 2}, {a: 1}, {a: 3}]}",
             "{output: [{a: 1}, {a: 2}]}");
}

TEST_F(SortStageTest, SortDescendingWithLimit) {
    testWork("{a: -1}",
             nullptr,
             "{}",
             2,
             "{input: [{a: 2}, {a: 1}, {a: 3}]}",
             "{output: [{a: 3}, {a: 2}]}");
}

//
// Sorting with limit > size of data set
// Implementation should retain top N items
// and discard the rest.
//

TEST_F(SortStageTest, SortAscendingWithLimitGreaterThanInputSize) {
    testWork("{a: 1}",
             nullptr,
             "{}",
             10,
             "{input: [{a: 2}, {a: 1}, {a: 3}]}",
             "{output: [{a: 1}, {a: 2}, {a: 3}]}");
}

TEST_F(SortStageTest, SortDescendingWithLimitGreaterThanInputSize) {
    testWork("{a: -1}",
             nullptr,
             "{}",
             10,
             "{input: [{a: 2}, {a: 1}, {a: 3}]}",
             "{output: [{a: 3}, {a: 2}, {a: 1}]}");
}

//
// Sorting with limit 1
// Implementation should optimize this into a running maximum.
//

TEST_F(SortStageTest, SortAscendingWithLimitOfOne) {
    testWork("{a: 1}", nullptr, "{}", 1, "{input: [{a: 2}, {a: 1}, {a: 3}]}", "{output: [{a: 1}]}");
}

TEST_F(SortStageTest, SortDescendingWithLimitOfOne) {
    testWork(
        "{a: -1}", nullptr, "{}", 1, "{input: [{a: 2}, {a: 1}, {a: 3}]}", "{output: [{a: 3}]}");
}

TEST_F(SortStageTest, SortAscendingWithCollation) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    testWork("{a: 1}",
             &collator,
             "{}",
             0,
             "{input: [{a: 'ba'}, {a: 'aa'}, {a: 'ab'}]}",
             "{output: [{a: 'aa'}, {a: 'ba'}, {a: 'ab'}]}");
}

TEST_F(SortStageTest, SortDescendingWithCollation) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    testWork("{a: -1}",
             &collator,
             "{}",
             0,
             "{input: [{a: 'ba'}, {a: 'aa'}, {a: 'ab'}]}",
             "{output: [{a: 'ab'}, {a: 'ba'}, {a: 'aa'}]}");
}
}  // namespace
