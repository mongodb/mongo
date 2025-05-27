/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

    ASSERT_EQ(PrepareConflictTracker::getGlobalNumPrepareConflicts(), 1);
    // 0 Because we did not advance the mock timer.
    ASSERT_EQ(_pct->getThisOpPrepareConflictDuration(), Milliseconds(0));
}

TEST_F(PrepareConflictTrackerTest, PrepareConflictCheckTimings) {
    _pct->beginPrepareConflict(*_tickSource);
    _tickSource->advance(Milliseconds(1000));
    _pct->updatePrepareConflict(*_tickSource);
    ASSERT_EQ(_pct->getThisOpPrepareConflictDuration(), Milliseconds(1000));
    _pct->endPrepareConflict(*_tickSource);
    ASSERT_EQ(PrepareConflictTracker::getGlobalWaitingForPrepareConflictsMicros(), 1000000);
    ASSERT_EQ(PrepareConflictTracker::getGlobalNumPrepareConflicts(), 1);
    ASSERT_EQ(_pct->getThisOpPrepareConflictDuration(), Milliseconds(1000));
}

// Check that multiple prepare conflicts are tracked appropriately.
// Within the same op, the per-operation duration and number of prepare conflicts should accumulate.
// Across ops, the global stats should accumulate.
TEST_F(PrepareConflictTrackerTest, ManyPrepareConflictsWithUpdates) {
    auto expectedDurationMillis = Milliseconds(0);
    for (int i = 1; i <= 5; i++) {
        _pct->beginPrepareConflict(*_tickSource);
        ASSERT_EQ(PrepareConflictTracker::getGlobalNumPrepareConflicts(), i);
        for (int j = 1; j <= 5; j++) {
            _tickSource->advance(Milliseconds(200));
            expectedDurationMillis += Milliseconds(200);
            _pct->updatePrepareConflict(*_tickSource);
            ASSERT_EQ(_pct->getThisOpPrepareConflictDuration(), expectedDurationMillis);
        }
        _pct->endPrepareConflict(*_tickSource);
        // Include the delay from the last test.
        ASSERT_EQ(PrepareConflictTracker::getGlobalWaitingForPrepareConflictsMicros(),
                  (expectedDurationMillis.count()) * 1000);
        ASSERT_EQ(_pct->getThisOpPrepareConflictCount(), i);
        ASSERT_EQ(_pct->getThisOpPrepareConflictDuration(), expectedDurationMillis);
    }
}

// Enforce the begin -> update(s) -> end ordering of the tracker.
DEATH_TEST_F(PrepareConflictTrackerTest, RejectBeginBeforeEnd, "invariant") {
    _pct->beginPrepareConflict(*_tickSource);
    _pct->beginPrepareConflict(*_tickSource);
}

DEATH_TEST_F(PrepareConflictTrackerTest, RejectBeginBeforeEndWithUpdate, "invariant") {
    _pct->beginPrepareConflict(*_tickSource);
    _pct->updatePrepareConflict(*_tickSource);
    _pct->beginPrepareConflict(*_tickSource);
}

DEATH_TEST_F(PrepareConflictTrackerTest, RejectUpdateBeforeBeginSteadyState, "invariant") {
    _pct->beginPrepareConflict(*_tickSource);
    _pct->endPrepareConflict(*_tickSource);
    _pct->updatePrepareConflict(*_tickSource);
}

DEATH_TEST_F(PrepareConflictTrackerTest, RejectEndBeforeBegin, "invariant") {
    _pct->beginPrepareConflict(*_tickSource);
    _pct->endPrepareConflict(*_tickSource);
    _pct->endPrepareConflict(*_tickSource);
}
}  // namespace
}  // namespace mongo
