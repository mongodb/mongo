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

#include "mongo/db/exec/classic/spool.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/db/exec/classic/mock_stage.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <cstdint>
#include <string>
#include <utility>
#include <variant>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

using namespace mongo;

namespace {

static const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("db.dummy");

class SpoolStageTest : public ServiceContextMongoDTest {
public:
    SpoolStageTest() : ServiceContextMongoDTest(Options{}.useMockClock(true)) {
        _opCtx = makeOperationContext();
        _expCtx = ExpressionContextBuilder{}.opCtx(_opCtx.get()).ns(kNss).build();
    }

    ExpressionContext* expCtx() {
        return _expCtx.get();
    }

    /**
     * Create a new working set member with the given record id.
     */
    WorkingSetID makeRecord(const std::variant<std::string, long>& recordId) {
        WorkingSetID id = ws.allocate();
        WorkingSetMember* wsm = ws.get(id);
        visit(OverloadedVisitor{
                  [&](long value) { wsm->recordId = RecordId(value); },
                  [&](const std::string& value) { wsm->recordId = RecordId(value); },
              },
              recordId);
        ws.transitionToRecordIdAndObj(id);
        return id;
    }

    /**
     * Helper that calls work() on the spool stage and validates the result according to the
     * expected values.
     */
    void workAndAssertStateAndRecordId(
        SpoolStage& spool,
        PlanStage::StageState expectedState,
        const std::variant<std::monostate, std::string, long>& expectedId = std::monostate{},
        bool childHasMoreRecords = true) {
        ASSERT_FALSE(spool.isEOF());

        WorkingSetID id = WorkingSet::INVALID_ID;
        auto state = spool.work(&id);
        ASSERT_EQUALS(state, expectedState);

        if (expectedId.index() != 0) {
            auto member = ws.get(id);
            ASSERT_TRUE(member->hasRecordId());
            visit(OverloadedVisitor{
                      [&](long value) {
                          ASSERT_TRUE(member->recordId.isLong());
                          ASSERT_EQUALS(member->recordId.getLong(), value);
                      },
                      [&](const std::string& value) {
                          ASSERT_TRUE(member->recordId.isStr());
                          ASSERT_EQUALS(member->recordId.getStr(), value);
                      },
                      [&](const std::monostate&) {},
                  },
                  expectedId);
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

    SpoolStage makeSpool(std::unique_ptr<PlanStage> root,
                         long long maxAllowedMemoryUsageBytes = 1024,
                         boost::optional<long long> maxAllowedDiskUsageBytes = boost::none) {
        if (maxAllowedDiskUsageBytes) {
            _tempDir = std::make_unique<unittest::TempDir>("SpoolStageTest");
            expCtx()->setTempDir(_tempDir->path());
            expCtx()->setAllowDiskUse(maxAllowedDiskUsageBytes.has_value());
        }

        RAIIServerParameterControllerForTest maxMemoryUsage("internalQueryMaxSpoolMemoryUsageBytes",
                                                            maxAllowedMemoryUsageBytes);
        RAIIServerParameterControllerForTest maxDiskUsage("internalQueryMaxSpoolDiskUsageBytes",
                                                          maxAllowedDiskUsageBytes.value_or(1));

        return SpoolStage(expCtx(), &ws, std::move(root));
    }

    WorkingSet ws;

private:
    ServiceContext::UniqueOperationContext _opCtx;
    boost::intrusive_ptr<ExpressionContext> _expCtx;
    std::unique_ptr<unittest::TempDir> _tempDir;

    long _memUsage = 0;
};

TEST_F(SpoolStageTest, eof) {
    auto mock = std::make_unique<MockStage>(expCtx(), &ws);

    auto spool = makeSpool(std::move(mock));
    assertEofState(spool);
    ASSERT_EQ(spool.getMemoryUsageTracker_forTest()->inUseTrackedMemoryBytes(), 0);
    ASSERT_EQ(spool.getMemoryUsageTracker_forTest()->peakTrackedMemoryBytes(), 0);
}

TEST_F(SpoolStageTest, basic) {
    auto mock = std::make_unique<MockStage>(expCtx(), &ws);
    mock->enqueueAdvanced(makeRecord(1));
    mock->enqueueAdvanced(makeRecord(2));
    mock->enqueueAdvanced(makeRecord(3));

    auto spool = makeSpool(std::move(mock));

    // There are no NEED_TIME/NEED_YIELDs to propagate so we can exhaust the input on the first call
    // to work() and then begin returning the cached results.
    workAndAssertStateAndRecordId(spool, PlanStage::ADVANCED, 1);
    workAndAssertStateAndRecordId(spool, PlanStage::ADVANCED, 2);
    workAndAssertStateAndRecordId(spool, PlanStage::ADVANCED, 3);
    assertEofState(spool);
    ASSERT_EQ(spool.getMemoryUsageTracker_forTest()->inUseTrackedMemoryBytes(), 0);
    ASSERT_GT(spool.getMemoryUsageTracker_forTest()->peakTrackedMemoryBytes(), 0);
}

TEST_F(SpoolStageTest, propagatesNeedTime) {
    auto mock = std::make_unique<MockStage>(expCtx(), &ws);
    mock->enqueueStateCode(PlanStage::NEED_TIME);
    mock->enqueueAdvanced(makeRecord(1));
    mock->enqueueStateCode(PlanStage::NEED_TIME);
    mock->enqueueAdvanced(makeRecord(2));
    mock->enqueueStateCode(PlanStage::NEED_TIME);
    mock->enqueueAdvanced(makeRecord(3));

    auto spool = makeSpool(std::move(mock));

    // First, consume all of the NEED_TIMEs from the child.
    workAndAssertStateAndRecordId(spool, PlanStage::NEED_TIME);
    workAndAssertStateAndRecordId(spool, PlanStage::NEED_TIME);
    workAndAssertStateAndRecordId(spool, PlanStage::NEED_TIME);

    // Now we can exhaust the child and start returning the cached results.
    for (long i = 1; i <= 3; ++i) {
        workAndAssertStateAndRecordId(spool, PlanStage::ADVANCED, i);
    }

    assertEofState(spool);

    ASSERT_EQ(spool.getMemoryUsageTracker_forTest()->inUseTrackedMemoryBytes(), 0);
    ASSERT_GT(spool.getMemoryUsageTracker_forTest()->peakTrackedMemoryBytes(), 0);
}

TEST_F(SpoolStageTest, propagatesNeedYield) {
    auto mock = std::make_unique<MockStage>(expCtx(), &ws);
    mock->enqueueAdvanced(makeRecord(1));
    mock->enqueueStateCode(PlanStage::NEED_YIELD);
    mock->enqueueAdvanced(makeRecord(2));
    mock->enqueueStateCode(PlanStage::NEED_YIELD);
    mock->enqueueStateCode(PlanStage::NEED_YIELD);
    mock->enqueueAdvanced(makeRecord(3));

    auto spool = makeSpool(std::move(mock));

    // First, consume all of the NEED_YIELDs from the child.
    workAndAssertStateAndRecordId(spool, PlanStage::NEED_YIELD);
    workAndAssertStateAndRecordId(spool, PlanStage::NEED_YIELD);
    workAndAssertStateAndRecordId(spool, PlanStage::NEED_YIELD);

    // Now we can exhaust the child and start returning the cached results.
    for (long i = 1; i <= 3; ++i) {
        workAndAssertStateAndRecordId(spool, PlanStage::ADVANCED, i);
    }

    assertEofState(spool);
    ASSERT_EQ(spool.getMemoryUsageTracker_forTest()->inUseTrackedMemoryBytes(), 0);
    ASSERT_GT(spool.getMemoryUsageTracker_forTest()->peakTrackedMemoryBytes(), 0);
}

TEST_F(SpoolStageTest, onlyNeedYieldAndNeedTime) {
    auto mock = std::make_unique<MockStage>(expCtx(), &ws);
    mock->enqueueStateCode(PlanStage::NEED_YIELD);
    mock->enqueueStateCode(PlanStage::NEED_TIME);
    mock->enqueueStateCode(PlanStage::NEED_YIELD);

    auto spool = makeSpool(std::move(mock));

    // Consume all the NEED_YIELD/NEED_TIMEs, then we should see EOF immediately
    workAndAssertStateAndRecordId(spool, PlanStage::NEED_YIELD);
    workAndAssertStateAndRecordId(spool, PlanStage::NEED_TIME);
    workAndAssertStateAndRecordId(
        spool, PlanStage::NEED_YIELD, std::monostate{}, false /* childHasMoreRecords */);

    assertEofState(spool);
    ASSERT_EQ(spool.getMemoryUsageTracker_forTest()->inUseTrackedMemoryBytes(), 0);
    ASSERT_EQ(spool.getMemoryUsageTracker_forTest()->peakTrackedMemoryBytes(), 0);
}

TEST_F(SpoolStageTest, spillEveryRecordId) {
    auto mock = std::make_unique<MockStage>(expCtx(), &ws);
    mock->enqueueAdvanced(makeRecord(1));
    mock->enqueueAdvanced(makeRecord(2));
    mock->enqueueAdvanced(makeRecord(3));

    const uint64_t maxAllowedMemoryUsageBytes = 1;
    auto spool =
        makeSpool(std::move(mock), maxAllowedMemoryUsageBytes, 1024 /* maxAllowedDiskUsageBytes */);

    workAndAssertStateAndRecordId(spool, PlanStage::ADVANCED, 1);
    workAndAssertStateAndRecordId(spool, PlanStage::ADVANCED, 2);
    workAndAssertStateAndRecordId(spool, PlanStage::ADVANCED, 3);
    assertEofState(spool);

    // Validate the spilling stats. We should have spilled for each record.
    auto stats = static_cast<const SpoolStats*>(spool.getSpecificStats());
    ASSERT_EQUALS(stats->spillingStats.getSpills(), 3);
    ASSERT_GREATER_THAN(stats->spillingStats.getSpilledDataStorageSize(), 0);
    ASSERT_EQUALS(stats->maxMemoryUsageBytes, maxAllowedMemoryUsageBytes);
    ASSERT_EQ(spool.getMemoryUsageTracker_forTest()->inUseTrackedMemoryBytes(), 0);
    ASSERT_GT(spool.getMemoryUsageTracker_forTest()->peakTrackedMemoryBytes(), 0);
}

TEST_F(SpoolStageTest, spillEveryOtherRecordId) {
    auto mock = std::make_unique<MockStage>(expCtx(), &ws);
    mock->enqueueAdvanced(makeRecord(1));
    mock->enqueueAdvanced(makeRecord(2));
    mock->enqueueAdvanced(makeRecord(3));
    mock->enqueueAdvanced(makeRecord(4));
    mock->enqueueAdvanced(makeRecord(5));

    const uint64_t maxAllowedMemoryUsageBytes = sizeof(RecordId) * 1.5;
    auto spool =
        makeSpool(std::move(mock), maxAllowedMemoryUsageBytes, 1024 /* maxAllowedDiskUsageBytes */);

    workAndAssertStateAndRecordId(spool, PlanStage::ADVANCED, 1);
    workAndAssertStateAndRecordId(spool, PlanStage::ADVANCED, 2);
    workAndAssertStateAndRecordId(spool, PlanStage::ADVANCED, 3);
    workAndAssertStateAndRecordId(spool, PlanStage::ADVANCED, 4);
    workAndAssertStateAndRecordId(spool, PlanStage::ADVANCED, 5);
    assertEofState(spool);

    // Validate the spilling stats. We should have spilled every other record.
    auto stats = static_cast<const SpoolStats*>(spool.getSpecificStats());
    ASSERT_EQUALS(stats->spillingStats.getSpills(), 2);
    ASSERT_EQUALS(stats->spillingStats.getSpilledRecords(), 4);
    ASSERT_GREATER_THAN(stats->spillingStats.getSpilledBytes(), 0);
    ASSERT_GREATER_THAN(stats->spillingStats.getSpilledDataStorageSize(), 0);
    ASSERT_EQUALS(stats->maxMemoryUsageBytes, maxAllowedMemoryUsageBytes);

    ASSERT_EQ(spool.getMemoryUsageTracker_forTest()->inUseTrackedMemoryBytes(), 0);
    ASSERT_GT(spool.getMemoryUsageTracker_forTest()->peakTrackedMemoryBytes(), 0);
}

TEST_F(SpoolStageTest, spillStringRecordId) {
    auto mock = std::make_unique<MockStage>(expCtx(), &ws);
    mock->enqueueAdvanced(makeRecord(1));
    mock->enqueueAdvanced(makeRecord("this is a short string"));
    mock->enqueueAdvanced(makeRecord(2));
    mock->enqueueAdvanced(makeRecord("this is a longer string........."));
    mock->enqueueAdvanced(makeRecord("the last string"));

    const uint64_t maxAllowedMemoryUsageBytes = sizeof(RecordId) + 1;
    auto spool =
        makeSpool(std::move(mock), maxAllowedMemoryUsageBytes, 1024 /* maxAllowedDiskUsageBytes */);

    workAndAssertStateAndRecordId(spool, PlanStage::ADVANCED, 1);
    workAndAssertStateAndRecordId(spool, PlanStage::ADVANCED, "this is a short string");
    workAndAssertStateAndRecordId(spool, PlanStage::ADVANCED, 2);
    workAndAssertStateAndRecordId(spool, PlanStage::ADVANCED, "this is a longer string.........");
    workAndAssertStateAndRecordId(spool, PlanStage::ADVANCED, "the last string");
    assertEofState(spool);

    // Validate the spilling stats. We should have spilled every other record.
    auto stats = static_cast<const SpoolStats*>(spool.getSpecificStats());
    ASSERT_EQUALS(stats->spillingStats.getSpills(), 2);
    ASSERT_EQUALS(stats->spillingStats.getSpilledRecords(), 4);
    ASSERT_GREATER_THAN(stats->spillingStats.getSpilledBytes(), 0);
    ASSERT_GREATER_THAN(stats->spillingStats.getSpilledDataStorageSize(), 0);
    ASSERT_EQUALS(stats->maxMemoryUsageBytes, maxAllowedMemoryUsageBytes);

    ASSERT_EQ(spool.getMemoryUsageTracker_forTest()->inUseTrackedMemoryBytes(), 0);
    ASSERT_GT(spool.getMemoryUsageTracker_forTest()->peakTrackedMemoryBytes(), 0);
}

TEST_F(SpoolStageTest, forceSpill) {
    auto mock = std::make_unique<MockStage>(expCtx(), &ws);
    mock->enqueueAdvanced(makeRecord(1));
    mock->enqueueAdvanced(makeRecord("this is a short string"));
    mock->enqueueAdvanced(makeRecord(2));
    mock->enqueueAdvanced(makeRecord("this is a longer string........."));
    mock->enqueueAdvanced(makeRecord("the last string"));

    const uint64_t maxAllowedMemoryUsageBytes = 100 * 1024 * 1024;
    auto spool =
        makeSpool(std::move(mock), maxAllowedMemoryUsageBytes, 1024 /* maxAllowedDiskUsageBytes */);

    workAndAssertStateAndRecordId(spool, PlanStage::ADVANCED, 1);
    workAndAssertStateAndRecordId(spool, PlanStage::ADVANCED, "this is a short string");
    spool.doForceSpill();
    workAndAssertStateAndRecordId(spool, PlanStage::ADVANCED, 2);
    workAndAssertStateAndRecordId(spool, PlanStage::ADVANCED, "this is a longer string.........");
    workAndAssertStateAndRecordId(spool, PlanStage::ADVANCED, "the last string");
    assertEofState(spool);

    // Validate the spilling stats. We should have spilled every other record.
    auto stats = static_cast<const SpoolStats*>(spool.getSpecificStats());
    ASSERT_EQUALS(stats->spillingStats.getSpills(), 1);
    ASSERT_EQUALS(stats->spillingStats.getSpilledRecords(), 3);
    ASSERT_GREATER_THAN(stats->spillingStats.getSpilledBytes(), 0);
    ASSERT_GREATER_THAN(stats->spillingStats.getSpilledDataStorageSize(), 0);
    ASSERT_EQUALS(stats->maxMemoryUsageBytes, maxAllowedMemoryUsageBytes);

    ASSERT_EQ(spool.getMemoryUsageTracker_forTest()->inUseTrackedMemoryBytes(), 0);
    ASSERT_GT(spool.getMemoryUsageTracker_forTest()->peakTrackedMemoryBytes(), 0);
}

TEST_F(SpoolStageTest, spillingDisabled) {
    auto mock = std::make_unique<MockStage>(expCtx(), &ws);
    mock->enqueueAdvanced(makeRecord(1));

    auto spool = makeSpool(std::move(mock),
                           1 /* maxAllowedMemoryUsageBytes */,
                           boost::none /* maxAllowedDiskUsageBytes */);

    WorkingSetID id;
    ASSERT_THROWS_CODE(
        spool.work(&id), AssertionException, ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed);
}

TEST_F(SpoolStageTest, maxDiskSpaceUsed) {
    auto mock = std::make_unique<MockStage>(expCtx(), &ws);
    mock->enqueueAdvanced(makeRecord(1));
    mock->enqueueAdvanced(makeRecord(2));

    auto spool = makeSpool(
        std::move(mock), 1 /* maxAllowedMemoryUsageBytes */, 1 /* maxAllowedDiskUsageBytes */);

    WorkingSetID id;
    ASSERT_THROWS_CODE(spool.work(&id), AssertionException, 7443700);
}

}  // namespace
