/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
 * This file contains tests for mongo/db/exec/spool.cpp
 */

#include "mongo/db/exec/mock_stage.h"
#include "mongo/db/exec/spool.h"
#include "mongo/db/service_context_d_test_fixture.h"

using namespace mongo;

namespace {

static const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("db.dummy");

class SpoolStageTest : public ServiceContextMongoDTest {
public:
    SpoolStageTest() : ServiceContextMongoDTest(Options{}.useMockClock(true)) {
        _opCtx = makeOperationContext();
        _expCtx = std::make_unique<ExpressionContext>(_opCtx.get(), nullptr, kNss);
    }

    ExpressionContext* expCtx() {
        return _expCtx.get();
    }

    /**
     * Create a new working set member with the given record id.
     */
    WorkingSetID makeRecord(long recordId) {
        WorkingSetID id = ws.allocate();
        WorkingSetMember* wsm = ws.get(id);
        wsm->recordId = RecordId(recordId);
        ws.transitionToRecordIdAndObj(id);
        return id;
    }

    /**
     * Helper that calls work() on the spool stage and validates the result according to the
     * expected values.
     */
    void workAndAssertStateAndRecordId(SpoolStage& spool,
                                       PlanStage::StageState expectedState,
                                       boost::optional<long> expectedId = boost::none,
                                       bool childHasMoreRecords = true) {
        ASSERT_FALSE(spool.isEOF());

        WorkingSetID id = WorkingSet::INVALID_ID;
        auto state = spool.work(&id);
        ASSERT_EQUALS(state, expectedState);

        if (expectedId) {
            auto member = ws.get(id);
            ASSERT_TRUE(member->hasRecordId());
            ASSERT_EQUALS(member->recordId.getLong(), *expectedId);
            _memUsage += member->recordId.memUsage();
        }

        // By spool definition, the child cannot have more records if we get ADVANCED or IS_EOF.
        // Whether the child is EOF for other states depends on whether it has more results.
        if (expectedState == PlanStage::ADVANCED) {
            ASSERT_TRUE(spool.child()->isEOF());
            ASSERT_FALSE(spool.isEOF());
        } else if (expectedState == PlanStage::IS_EOF) {
            ASSERT_TRUE(spool.child()->isEOF());
            ASSERT_TRUE(spool.isEOF());
        } else {
            ASSERT_EQUALS(spool.child()->isEOF(), !childHasMoreRecords);
        }
    }

    void assertEofState(SpoolStage& spool) {
        workAndAssertStateAndRecordId(spool, PlanStage::IS_EOF);

        auto stats = static_cast<const SpoolStats*>(spool.getSpecificStats());
        ASSERT_EQUALS(stats->totalDataSizeBytes, _memUsage);
    }

    WorkingSet ws;

private:
    ServiceContext::UniqueOperationContext _opCtx;
    std::unique_ptr<ExpressionContext> _expCtx;

    long _memUsage = 0;
};

TEST_F(SpoolStageTest, eof) {
    auto mock = std::make_unique<MockStage>(expCtx(), &ws);

    auto spool = SpoolStage(expCtx(), &ws, std::move(mock));
    assertEofState(spool);
}

TEST_F(SpoolStageTest, basic) {
    std::vector<WorkingSetID> docs{makeRecord(1), makeRecord(2), makeRecord(3)};
    auto mock = std::make_unique<MockStage>(expCtx(), &ws);
    mock->enqueueAdvanced(docs[0]);
    mock->enqueueAdvanced(docs[1]);
    mock->enqueueAdvanced(docs[2]);

    auto spool = SpoolStage(expCtx(), &ws, std::move(mock));

    // There are no NEED_TIME/NEED_YIELDs to propagate so we can exhaust the input on the first call
    // to work() and then begin returning the cached results.
    workAndAssertStateAndRecordId(spool, PlanStage::ADVANCED, 1);
    workAndAssertStateAndRecordId(spool, PlanStage::ADVANCED, 2);
    workAndAssertStateAndRecordId(spool, PlanStage::ADVANCED, 3);
    assertEofState(spool);
}

TEST_F(SpoolStageTest, propagatesNeedTime) {
    std::vector<WorkingSetID> docs{makeRecord(1), makeRecord(2), makeRecord(3)};
    auto mock = std::make_unique<MockStage>(expCtx(), &ws);
    mock->enqueueStateCode(PlanStage::NEED_TIME);
    mock->enqueueAdvanced(docs[0]);
    mock->enqueueStateCode(PlanStage::NEED_TIME);
    mock->enqueueAdvanced(docs[1]);
    mock->enqueueStateCode(PlanStage::NEED_TIME);
    mock->enqueueAdvanced(docs[2]);

    auto spool = SpoolStage(expCtx(), &ws, std::move(mock));

    // First, consume all of the NEED_TIMEs from the child.
    workAndAssertStateAndRecordId(spool, PlanStage::NEED_TIME);
    workAndAssertStateAndRecordId(spool, PlanStage::NEED_TIME);
    workAndAssertStateAndRecordId(spool, PlanStage::NEED_TIME);

    // Now we can exhaust the child and start returning the cached results.
    for (long i = 1; i <= 3; ++i) {
        workAndAssertStateAndRecordId(spool, PlanStage::ADVANCED, i);
    }

    assertEofState(spool);
}

TEST_F(SpoolStageTest, propagatesNeedYield) {
    std::vector<WorkingSetID> docs{makeRecord(1), makeRecord(2), makeRecord(3)};
    auto mock = std::make_unique<MockStage>(expCtx(), &ws);
    mock->enqueueAdvanced(docs[0]);
    mock->enqueueStateCode(PlanStage::NEED_YIELD);
    mock->enqueueAdvanced(docs[1]);
    mock->enqueueStateCode(PlanStage::NEED_YIELD);
    mock->enqueueStateCode(PlanStage::NEED_YIELD);
    mock->enqueueAdvanced(docs[2]);

    auto spool = SpoolStage(expCtx(), &ws, std::move(mock));

    // First, consume all of the NEED_YIELDs from the child.
    workAndAssertStateAndRecordId(spool, PlanStage::NEED_YIELD);
    workAndAssertStateAndRecordId(spool, PlanStage::NEED_YIELD);
    workAndAssertStateAndRecordId(spool, PlanStage::NEED_YIELD);

    // Now we can exhaust the child and start returning the cached results.
    for (long i = 1; i <= 3; ++i) {
        workAndAssertStateAndRecordId(spool, PlanStage::ADVANCED, i);
    }

    assertEofState(spool);
}

TEST_F(SpoolStageTest, onlyNeedYieldAndNeedTime) {
    auto mock = std::make_unique<MockStage>(expCtx(), &ws);
    mock->enqueueStateCode(PlanStage::NEED_YIELD);
    mock->enqueueStateCode(PlanStage::NEED_TIME);
    mock->enqueueStateCode(PlanStage::NEED_YIELD);

    auto spool = SpoolStage(expCtx(), &ws, std::move(mock));

    // Consume all the NEED_YIELD/NEED_TIMEs, then we should see EOF immediately
    workAndAssertStateAndRecordId(spool, PlanStage::NEED_YIELD);
    workAndAssertStateAndRecordId(spool, PlanStage::NEED_TIME);
    workAndAssertStateAndRecordId(
        spool, PlanStage::NEED_YIELD, boost::none, false /* childHasMoreRecords */);

    assertEofState(spool);
}
}  // namespace
