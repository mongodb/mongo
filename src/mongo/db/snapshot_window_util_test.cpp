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

#include "mongo/db/snapshot_window_util.h"

#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_devnull_test_fixture.h"
#include "mongo/db/snapshot_window_options.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using namespace SnapshotWindowUtil;

/**
 * Tests the functions in snapshot_window_util.h using a devnull storage engine.
 */
class SnapshotWindowTest : public ServiceContextDevnullTestFixture {
public:
    void setUp() override {
        ServiceContextDevnullTestFixture::setUp();
        _opCtx = cc().makeOperationContext();
    }

    void tearDown() override {
        _opCtx.reset();
        ServiceContextDevnullTestFixture::tearDown();
    }

    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(SnapshotWindowTest, DecreaseAndIncreaseSnapshotWindow) {
    auto engine = getServiceContext()->getStorageEngine();
    invariant(engine);

    snapshotWindowParams.maxTargetSnapshotHistoryWindowInSeconds.store(100);
    snapshotWindowParams.targetSnapshotHistoryWindowInSeconds.store(100);

    // Lower the time enforced between function calls to speed up testing.
    snapshotWindowParams.minMillisBetweenSnapshotWindowInc.store(100);

    // Stabilize for the test so we know that we are testing things as expected.
    snapshotWindowParams.snapshotWindowAdditiveIncreaseSeconds.store(2);
    snapshotWindowParams.snapshotWindowMultiplicativeDecrease.store(0.75);

    auto maxTargetSnapshotWindowSeconds =
        snapshotWindowParams.maxTargetSnapshotHistoryWindowInSeconds.load();
    auto snapshotWindowSeconds = snapshotWindowParams.targetSnapshotHistoryWindowInSeconds.load();
    ASSERT_EQ(maxTargetSnapshotWindowSeconds, snapshotWindowSeconds);

    auto windowMultiplicativeDecrease =
        snapshotWindowParams.snapshotWindowMultiplicativeDecrease.load();
    auto windowAdditiveIncrease = snapshotWindowParams.snapshotWindowAdditiveIncreaseSeconds.load();

    auto minTimeBetweenInc = snapshotWindowParams.minMillisBetweenSnapshotWindowInc.load();

    /**
     * Test that trying to decrease the window size FAILS when there have been no writes to the
     * cache overflow table.
     */

    decreaseTargetSnapshotWindowSize(_opCtx.get());
    auto snapshotWindowSecondsOne =
        snapshotWindowParams.targetSnapshotHistoryWindowInSeconds.load();

    ASSERT_EQ(snapshotWindowSeconds, snapshotWindowSecondsOne);

    /**
     * Test that trying to decrease the window size SUCCEEDS when there have been writes to the
     * cache overflow table.
     */

    engine->setCacheOverflowTableInsertCountForTest(1);

    decreaseTargetSnapshotWindowSize(_opCtx.get());
    auto snapshotWindowSecondsTwo =
        snapshotWindowParams.targetSnapshotHistoryWindowInSeconds.load();

    ASSERT_GT(snapshotWindowSecondsOne, snapshotWindowSecondsTwo);
    ASSERT_EQ(snapshotWindowSecondsTwo,
              static_cast<int>(snapshotWindowSeconds * windowMultiplicativeDecrease));

    /**
     * Test that trying to decrease the window size FAILS when there have been writes to the
     * cache overflow table AND SnapshotTooOld errors have occurred.
     */

    engine->setCacheOverflowTableInsertCountForTest(2);
    incrementSnapshotTooOldErrorCount();

    decreaseTargetSnapshotWindowSize(_opCtx.get());
    auto snapshotWindowSecondsThree =
        snapshotWindowParams.targetSnapshotHistoryWindowInSeconds.load();

    ASSERT_EQ(snapshotWindowSecondsTwo, snapshotWindowSecondsThree);

    /**
     * Now test again that decreasing the size SUCCEEDS when there have been writes to the cache
     * overflow table again (without any further SnapshotTooOld errors).
     */

    engine->setCacheOverflowTableInsertCountForTest(3);

    decreaseTargetSnapshotWindowSize(_opCtx.get());
    auto snapshotWindowSecondsFour =
        snapshotWindowParams.targetSnapshotHistoryWindowInSeconds.load();

    ASSERT_GT(snapshotWindowSecondsThree, snapshotWindowSecondsFour);
    ASSERT_EQ(snapshotWindowSecondsFour,
              static_cast<int>(snapshotWindowSecondsThree * windowMultiplicativeDecrease));

    /**
     * Test that increasing the size SUCCEEDS.
     */

    increaseTargetSnapshotWindowSize(_opCtx.get());
    auto snapshotWindowSecondsFive =
        snapshotWindowParams.targetSnapshotHistoryWindowInSeconds.load();

    ASSERT_EQ(snapshotWindowSecondsFive, snapshotWindowSecondsFour + windowAdditiveIncrease);

    /**
     * Test that increasing the size SUCCEEDS even when there have been writes to the cache overflow
     * table.
     */

    // Sleep for a time because increaseTargetSnapshotWindowSize() enforces a wait time between
    // updates.
    sleepmillis(2 * minTimeBetweenInc);

    engine->setCacheOverflowTableInsertCountForTest(4);

    increaseTargetSnapshotWindowSize(_opCtx.get());
    auto snapshotWindowSecondsSix =
        snapshotWindowParams.targetSnapshotHistoryWindowInSeconds.load();

    ASSERT_EQ(snapshotWindowSecondsSix, snapshotWindowSecondsFive + windowAdditiveIncrease);

    /**
     * Test that the size cannot be increased above the maximum size.
     */

    // Bump up the additive increase to make this run faster.
    snapshotWindowParams.snapshotWindowAdditiveIncreaseSeconds.store(9);
    windowAdditiveIncrease = snapshotWindowParams.snapshotWindowAdditiveIncreaseSeconds.load();

    // Integers round down, so add 1 to make sure it reaches the max.
    int numIncreasesToReachMax =
        (maxTargetSnapshotWindowSeconds - snapshotWindowSecondsSix) / windowAdditiveIncrease + 1;
    for (int i = 0; i < numIncreasesToReachMax; ++i) {
        sleepmillis(2 * minTimeBetweenInc);
        increaseTargetSnapshotWindowSize(_opCtx.get());
    }

    // Should be at max.
    auto snapshotWindowSecondsSeven =
        snapshotWindowParams.targetSnapshotHistoryWindowInSeconds.load();
    ASSERT_EQ(snapshotWindowSecondsSeven, maxTargetSnapshotWindowSeconds);

    // An attempt to increase beyond max should have no effect.
    sleepmillis(2 * minTimeBetweenInc);
    increaseTargetSnapshotWindowSize(_opCtx.get());
    auto snapshotWindowSecondsEight =
        snapshotWindowParams.targetSnapshotHistoryWindowInSeconds.load();
    ASSERT_EQ(snapshotWindowSecondsEight, maxTargetSnapshotWindowSeconds);
}

}  // namespace
}  // namespace mongo
