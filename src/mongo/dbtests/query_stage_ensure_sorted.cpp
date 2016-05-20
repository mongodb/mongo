/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/db/exec/ensure_sorted.h"
#include "mongo/db/exec/queued_data_stage.h"
#include "mongo/db/exec/sort_key_generator.h"
#include "mongo/db/json.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

class QueryStageEnsureSortedTest : public unittest::Test {
public:
    /**
     * Test function to verify the EnsureSortedStage.
     * patternStr is the JSON representation of the sort pattern BSONObj.
     * inputStr represents the input data set in a BSONObj.
     *     {input: [doc1, doc2, doc3, ...]}
     * expectedStr represents the expected output data set.
     *     {output: [docA, docB, docC, ...]}
     * collator is passed to EnsureSortedStage() for string comparisons.
     */
    void testWork(const char* patternStr,
                  const char* inputStr,
                  const char* expectedStr,
                  CollatorInterface* collator = nullptr) {
        auto txn = _serviceContext.makeOperationContext();

        WorkingSet ws;
        auto queuedDataStage = stdx::make_unique<QueuedDataStage>(txn.get(), &ws);
        BSONObj inputObj = fromjson(inputStr);
        BSONElement inputElt = inputObj["input"];
        ASSERT(inputElt.isABSONObj());

        for (auto&& elt : inputElt.embeddedObject()) {
            ASSERT(elt.isABSONObj());
            BSONObj obj = elt.embeddedObject().getOwned();

            // Insert obj from input array into working set.
            WorkingSetID id = ws.allocate();
            WorkingSetMember* wsm = ws.get(id);
            wsm->obj = Snapshotted<BSONObj>(SnapshotId(), obj);
            wsm->transitionToOwnedObj();
            queuedDataStage->pushBack(id);
        }

        // Initialization.
        BSONObj pattern = fromjson(patternStr);
        auto sortKeyGen = stdx::make_unique<SortKeyGeneratorStage>(
            txn.get(), queuedDataStage.release(), &ws, pattern, BSONObj(), collator);
        EnsureSortedStage ess(txn.get(), pattern, &ws, sortKeyGen.release());
        WorkingSetID id = WorkingSet::INVALID_ID;
        PlanStage::StageState state = PlanStage::NEED_TIME;

        // Documents are inserted into BSON document in this format:
        //     {output: [docA, docB, docC, ...]}
        BSONObjBuilder bob;
        BSONArrayBuilder arr(bob.subarrayStart("output"));
        while (state != PlanStage::IS_EOF) {
            state = ess.work(&id);
            ASSERT_NE(state, PlanStage::DEAD);
            ASSERT_NE(state, PlanStage::FAILURE);
            if (state == PlanStage::ADVANCED) {
                WorkingSetMember* member = ws.get(id);
                const BSONObj& obj = member->obj.value();
                arr.append(obj);
            }
        }
        ASSERT_TRUE(ess.isEOF());
        arr.doneFast();
        BSONObj outputObj = bob.obj();

        // Compare the results against what we expect.
        BSONObj expectedObj = fromjson(expectedStr);
        ASSERT_EQ(outputObj, expectedObj);
    }

protected:
    QueryTestServiceContext _serviceContext;
};

TEST_F(QueryStageEnsureSortedTest, EnsureSortedEmptyWorkingSet) {
    auto txn = _serviceContext.makeOperationContext();

    WorkingSet ws;
    auto queuedDataStage = stdx::make_unique<QueuedDataStage>(txn.get(), &ws);
    auto sortKeyGen = stdx::make_unique<SortKeyGeneratorStage>(
        txn.get(), queuedDataStage.release(), &ws, BSONObj(), BSONObj(), nullptr);
    EnsureSortedStage ess(txn.get(), BSONObj(), &ws, sortKeyGen.release());

    WorkingSetID id = WorkingSet::INVALID_ID;
    PlanStage::StageState state = PlanStage::NEED_TIME;
    while (state == PlanStage::NEED_TIME || state == PlanStage::NEED_YIELD) {
        state = ess.work(&id);
        ASSERT_NE(state, PlanStage::ADVANCED);
    }
    ASSERT_EQ(state, PlanStage::IS_EOF);
}

//
// EnsureSorted on already sorted order should make no change.
//

TEST_F(QueryStageEnsureSortedTest, EnsureAlreadySortedAscending) {
    testWork("{a: 1}", "{input: [{a: 1}, {a: 2}, {a: 3}]}", "{output: [{a: 1}, {a: 2}, {a: 3}]}");
}

TEST_F(QueryStageEnsureSortedTest, EnsureAlreadySortedDescending) {
    testWork("{a: -1}", "{input: [{a: 3}, {a: 2}, {a: 1}]}", "{output: [{a: 3}, {a: 2}, {a: 1}]}");
}

TEST_F(QueryStageEnsureSortedTest, EnsureIrrelevantSortKey) {
    testWork("{b: 1}", "{input: [{a: 2}, {a: 1}, {a: 3}]}", "{output: [{a: 2}, {a: 1}, {a: 3}]}");
}

//
// EnsureSorted should drop unsorted results.
//

TEST_F(QueryStageEnsureSortedTest, EnsureSortedOnAscending) {
    testWork("{a: 1}",
             "{input: [{a: 1}, {a: 2}, {a: 0}, {a: 4}, {a: 6}]}",
             "{output: [{a: 1}, {a: 2}, {a: 4}, {a: 6}]}");
}

TEST_F(QueryStageEnsureSortedTest, EnsureSortedOnDescending) {
    testWork("{a: -1}",
             "{input: [{a: 6}, {a: 4}, {a: 3}, {a: 9}, {a: 8}]}",
             "{output: [{a: 6}, {a: 4}, {a: 3}]}");
}

TEST_F(QueryStageEnsureSortedTest, EnsureSortedCompoundKey) {
    testWork("{a: -1, b: 1}",
             "{input: [{a: 6, b: 10}, {a: 6, b: 8}, {a: 6, b: 12}, {a: 9, b: 13}, {a: 5, b: 1}]}",
             "{output: [{a: 6, b: 10}, {a: 6, b: 12}, {a: 5, b: 1}]}");
}

TEST_F(QueryStageEnsureSortedTest, EnsureSortedStringsNullCollator) {
    testWork("{a: 1}", "{input: [{a: 'abc'}, {a: 'cba'}]}", "{output: [{a: 'abc'}, {a: 'cba'}]}");
}

TEST_F(QueryStageEnsureSortedTest, EnsureSortedStringsCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    testWork("{a: 1}", "{input: [{a: 'abc'}, {a: 'cba'}]}", "{output: [{a: 'abc'}]}", &collator);
}

}  // namespace

}  // namespace mongo
