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

/**
 * This file contains tests for mongo/db/exec/sort.cpp
 */

#include "mongo/db/exec/classic/sort.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/exec/classic/queued_data_stage.h"
#include "mongo/db/exec/classic/sort_key_generator.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_factory_mock.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <memory>
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

using namespace mongo;

namespace {

static const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("db.dummy");

class SortStageDefaultTest : public ServiceContextMongoDTest {
public:
    static constexpr uint64_t kMaxMemoryUsageBytes = 1024u * 1024u;

    SortStageDefaultTest() : ServiceContextMongoDTest(Options{}.useMockClock(true)) {
        _opCtx = makeOperationContext();
        CollatorFactoryInterface::set(getServiceContext(), std::make_unique<CollatorFactoryMock>());
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    /**
     * Test function to verify sort stage. SortStageDefault will be initialized using 'patternStr',
     * 'collator', and 'limit;.
     *
     * 'inputStr' represents the input data set in a BSONObj.
     *     {input: [doc1, doc2, doc3, ...]}
     *
     * 'expectedStr; represents the expected sorted data set.
     *     {output: [docA, docB, docC, ...]}
     */
    void testWork(const char* patternStr,
                  CollatorInterface* collator,
                  int limit,
                  const char* inputStr,
                  const char* expectedStr) {
        // WorkingSet is not owned by stages
        // so it's fine to declare
        WorkingSet ws;

        auto expCtx = ExpressionContextBuilder{}
                          .opCtx(opCtx())
                          .collator(CollatorInterface::cloneCollator(collator))
                          .ns(kNss)
                          .build();
        // QueuedDataStage will be owned by SortStageDefault.
        auto queuedDataStage = std::make_unique<QueuedDataStage>(expCtx.get(), &ws);
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
            wsm->doc = {SnapshotId(), Document{obj}};
            wsm->transitionToOwnedObj();
            queuedDataStage->pushBack(id);
        }

        auto sortPattern = fromjson(patternStr);

        auto sortKeyGen = std::make_unique<SortKeyGeneratorStage>(
            expCtx, std::move(queuedDataStage), &ws, sortPattern);

        SortStageDefault sort(expCtx,
                              &ws,
                              SortPattern{sortPattern, expCtx},
                              limit,
                              kMaxMemoryUsageBytes,
                              false,  // addSortKeyMetadata
                              std::move(sortKeyGen));

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
            BSONObj obj = member->doc.value().toBson();
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
        if (SimpleBSONObjComparator::kInstance.evaluate(outputObj != expectedObj)) {
            str::stream ss;
            // Even though we have the original string representation of the expected output,
            // we invoke BSONObj::toString() to get a format consistent with outputObj.
            ss << "Unexpected sort result with pattern=" << patternStr << "; limit=" << limit
               << ":\n"
               << "Expected: " << expectedObj.toString() << "\n"
               << "Actual:   " << outputObj.toString() << "\n";
            FAIL(std::string(ss));
        }
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(SortStageDefaultTest, SortEmptyWorkingSet) {
    WorkingSet ws;
    auto expCtx = ExpressionContextBuilder{}.opCtx(opCtx()).ns(kNss).build();
    // QueuedDataStage will be owned by SortStageDefault.
    auto queuedDataStage = std::make_unique<QueuedDataStage>(expCtx.get(), &ws);
    auto sortKeyGen =
        std::make_unique<SortKeyGeneratorStage>(expCtx, std::move(queuedDataStage), &ws, BSONObj());
    auto sortPattern = BSON("a" << 1);
    SortStageDefault sort(expCtx,
                          &ws,
                          SortPattern{sortPattern, expCtx},
                          0u,
                          kMaxMemoryUsageBytes,
                          false,  // addSortKeyMetadata
                          std::move(sortKeyGen));

    // Check initial EOF state.
    ASSERT_FALSE(sort.isEOF());

    // First call to work() sorts data in vector.
    WorkingSetID id = WorkingSet::INVALID_ID;
    auto state = sort.work(&id);
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

TEST_F(SortStageDefaultTest, SortAscending) {
    testWork("{a: 1}",
             nullptr,
             0,
             "{input: [{a: 2}, {a: 1}, {a: 3}]}",
             "{output: [{a: 1}, {a: 2}, {a: 3}]}");
}

TEST_F(SortStageDefaultTest, SortDescending) {
    testWork("{a: -1}",
             nullptr,
             0,
             "{input: [{a: 2}, {a: 1}, {a: 3}]}",
             "{output: [{a: 3}, {a: 2}, {a: 1}]}");
}

TEST_F(SortStageDefaultTest, SortIrrelevantSortKey) {
    testWork("{b: 1}",
             nullptr,
             0,
             "{input: [{a: 2}, {a: 1}, {a: 3}]}",
             "{output: [{a: 2}, {a: 1}, {a: 3}]}");
}

//
// Sorting with limit > 1
// Implementation should retain top N items
// and discard the rest.
//

TEST_F(SortStageDefaultTest, SortAscendingWithLimit) {
    testWork(
        "{a: 1}", nullptr, 2, "{input: [{a: 2}, {a: 1}, {a: 3}]}", "{output: [{a: 1}, {a: 2}]}");
}

TEST_F(SortStageDefaultTest, SortDescendingWithLimit) {
    testWork(
        "{a: -1}", nullptr, 2, "{input: [{a: 2}, {a: 1}, {a: 3}]}", "{output: [{a: 3}, {a: 2}]}");
}

//
// Sorting with limit > size of data set
// Implementation should retain top N items
// and discard the rest.
//

TEST_F(SortStageDefaultTest, SortAscendingWithLimitGreaterThanInputSize) {
    testWork("{a: 1}",
             nullptr,
             10,
             "{input: [{a: 2}, {a: 1}, {a: 3}]}",
             "{output: [{a: 1}, {a: 2}, {a: 3}]}");
}

TEST_F(SortStageDefaultTest, SortDescendingWithLimitGreaterThanInputSize) {
    testWork("{a: -1}",
             nullptr,
             10,
             "{input: [{a: 2}, {a: 1}, {a: 3}]}",
             "{output: [{a: 3}, {a: 2}, {a: 1}]}");
}

//
// Sorting with limit 1
// Implementation should optimize this into a running maximum.
//

TEST_F(SortStageDefaultTest, SortAscendingWithLimitOfOne) {
    testWork("{a: 1}", nullptr, 1, "{input: [{a: 2}, {a: 1}, {a: 3}]}", "{output: [{a: 1}]}");
}

TEST_F(SortStageDefaultTest, SortDescendingWithLimitOfOne) {
    testWork("{a: -1}", nullptr, 1, "{input: [{a: 2}, {a: 1}, {a: 3}]}", "{output: [{a: 3}]}");
}

TEST_F(SortStageDefaultTest, SortAscendingWithCollation) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    testWork("{a: 1}",
             &collator,
             0,
             "{input: [{a: 'ba'}, {a: 'aa'}, {a: 'ab'}]}",
             "{output: [{a: 'aa'}, {a: 'ba'}, {a: 'ab'}]}");
}

TEST_F(SortStageDefaultTest, SortDescendingWithCollation) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    testWork("{a: -1}",
             &collator,
             0,
             "{input: [{a: 'ba'}, {a: 'aa'}, {a: 'ab'}]}",
             "{output: [{a: 'ab'}, {a: 'ba'}, {a: 'aa'}]}");
}
}  // namespace
