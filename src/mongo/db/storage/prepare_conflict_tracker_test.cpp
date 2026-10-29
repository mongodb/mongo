// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/prepare_conflict_tracker.h"

#include "mongo/db/service_context.h"
#include "mongo/db/storage/execution_context.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/tick_source_mock.h"

#include <string>

namespace mongo {
namespace {

class PrepareConflictTrackerTest : public unittest::Test {
public:
    void setUp() override {
        auto tickSource = std::make_unique<TickSourceMock<>>();
        tickSource->advance(Milliseconds(10));
        _tickSource = tickSource.get();
        setGlobalServiceContext(ServiceContext::make(nullptr, nullptr, std::move(tickSource)));
        auto serviceContext = getGlobalServiceContext();
        _client = serviceContext->getService()->makeClient("myClient");
        _opCtx = serviceContext->makeOperationContext(_client.get());
        _pct = &StorageExecutionContext::get(_opCtx.get())->getPrepareConflictTracker();
        _pct->resetGlobalPrepareConflictStats();
    }

protected:
    unittest::TempDir _home{"temp"};
    ServiceContext::UniqueClient _client;
    ServiceContext::UniqueOperationContext _opCtx;
    TickSourceMock<>* _tickSource = nullptr;
    PrepareConflictTracker* _pct = nullptr;
};

// Tests that normal operations give expected behavior.
TEST_F(PrepareConflictTrackerTest, BeginAndEndPrepareConflict) {
    _pct->beginPrepareConflict(*_tickSource);
    _pct->endPrepareConflict(*_tickSource);

    EXPECT_EQ(PrepareConflictTracker::getGlobalNumPrepareConflicts(), 1);
    // 0 Because we did not advance the mock timer.
    EXPECT_EQ(_pct->getThisOpPrepareConflictDuration(), Milliseconds(0));
}

TEST_F(PrepareConflictTrackerTest, PrepareConflictCheckTimings) {
    _pct->beginPrepareConflict(*_tickSource);
    _tickSource->advance(Milliseconds(1000));
    _pct->updatePrepareConflict(*_tickSource);
    EXPECT_EQ(_pct->getThisOpPrepareConflictDuration(), Milliseconds(1000));
    _pct->endPrepareConflict(*_tickSource);
    EXPECT_EQ(PrepareConflictTracker::getGlobalWaitingForPrepareConflictsMicros(), 1000000);
    EXPECT_EQ(PrepareConflictTracker::getGlobalNumPrepareConflicts(), 1);
    EXPECT_EQ(_pct->getThisOpPrepareConflictDuration(), Milliseconds(1000));
}

// Check that multiple prepare conflicts are tracked appropriately.
// Within the same op, the per-operation duration and number of prepare conflicts should accumulate.
// Across ops, the global stats should accumulate.
TEST_F(PrepareConflictTrackerTest, ManyPrepareConflictsWithUpdates) {
    auto expectedDurationMillis = Milliseconds(0);
    for (int i = 1; i <= 5; i++) {
        _pct->beginPrepareConflict(*_tickSource);
        EXPECT_EQ(PrepareConflictTracker::getGlobalNumPrepareConflicts(), i);
        for (int j = 1; j <= 5; j++) {
            _tickSource->advance(Milliseconds(200));
            expectedDurationMillis += Milliseconds(200);
            _pct->updatePrepareConflict(*_tickSource);
            EXPECT_EQ(_pct->getThisOpPrepareConflictDuration(), expectedDurationMillis);
        }
        _pct->endPrepareConflict(*_tickSource);
        // Include the delay from the last test.
        EXPECT_EQ(PrepareConflictTracker::getGlobalWaitingForPrepareConflictsMicros(),
                  (expectedDurationMillis.count()) * 1000);
        EXPECT_EQ(_pct->getThisOpPrepareConflictCount(), i);
        EXPECT_EQ(_pct->getThisOpPrepareConflictDuration(), expectedDurationMillis);
    }
}

// Enforce the begin -> update(s) -> end ordering of the tracker.
using PrepareConflictTrackerTestDeathTest = PrepareConflictTrackerTest;
DEATH_TEST_F(PrepareConflictTrackerTestDeathTest, RejectBeginBeforeEnd, "invariant") {
    _pct->beginPrepareConflict(*_tickSource);
    _pct->beginPrepareConflict(*_tickSource);
}

DEATH_TEST_F(PrepareConflictTrackerTestDeathTest, RejectBeginBeforeEndWithUpdate, "invariant") {
    _pct->beginPrepareConflict(*_tickSource);
    _pct->updatePrepareConflict(*_tickSource);
    _pct->beginPrepareConflict(*_tickSource);
}

DEATH_TEST_F(PrepareConflictTrackerTestDeathTest, RejectUpdateBeforeBeginSteadyState, "invariant") {
    _pct->beginPrepareConflict(*_tickSource);
    _pct->endPrepareConflict(*_tickSource);
    _pct->updatePrepareConflict(*_tickSource);
}

DEATH_TEST_F(PrepareConflictTrackerTestDeathTest, RejectEndBeforeBegin, "invariant") {
    _pct->beginPrepareConflict(*_tickSource);
    _pct->endPrepareConflict(*_tickSource);
    _pct->endPrepareConflict(*_tickSource);
}
}  // namespace
}  // namespace mongo
