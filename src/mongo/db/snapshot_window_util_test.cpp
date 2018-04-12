/**
 *    Copyright (C) 2018 10gen Inc.
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

    // Lower the time enforced between function calls to speed up testing.
    // Dec must match Inc b/c increaseTargetWindowSize can call into decreaseTargetWindowSize.
    snapshotWindowParams.minMillisBetweenSnapshotWindowInc.store(100);
    snapshotWindowParams.minMillisBetweenSnapshotWindowDec.store(100);

    auto maxTargetSnapshotWindowSeconds =
        snapshotWindowParams.maxTargetSnapshotHistoryWindowInSeconds.load();
    auto snapshotWindowSeconds = snapshotWindowParams.targetSnapshotHistoryWindowInSeconds.load();
    ASSERT_EQ(maxTargetSnapshotWindowSeconds, snapshotWindowSeconds);

    auto windowMultiplicativeDecrease =
        snapshotWindowParams.snapshotWindowMultiplicativeDecrease.load();
    auto windowAdditiveIncrease = snapshotWindowParams.snapshotWindowAdditiveIncreaseSeconds.load();

    auto cachePressureThreshold = snapshotWindowParams.cachePressureThreshold.load();
    auto minTimeBetweenInc = snapshotWindowParams.minMillisBetweenSnapshotWindowInc.load();

    /**
     * Test that decreasing the size succeeds when cache pressure is ABOVE the threshold
     */

    engine->setCachePressureForTest(cachePressureThreshold + 5);

    decreaseTargetSnapshotWindowSize(_opCtx.get());
    auto snapshotWindowSecondsOne =
        snapshotWindowParams.targetSnapshotHistoryWindowInSeconds.load();

    ASSERT_GT(snapshotWindowSeconds, snapshotWindowSecondsOne);
    ASSERT_EQ(snapshotWindowSecondsOne,
              static_cast<int>(snapshotWindowSeconds * windowMultiplicativeDecrease));

    /**
     * Test that increasing the size SUCCEEDS when the cache pressure is BELOW the threshold.
     */

    engine->setCachePressureForTest(cachePressureThreshold - 5);

    increaseTargetSnapshotWindowSize(_opCtx.get());
    auto snapshotWindowSecondsTwo =
        snapshotWindowParams.targetSnapshotHistoryWindowInSeconds.load();

    ASSERT_EQ(snapshotWindowSecondsTwo, snapshotWindowSecondsOne + windowAdditiveIncrease);

    /**
     * Test that increasing the size FAILS when the cache pressure is ABOVE the threshold, and
     * instead this causes the size to be decreased.
     */

    engine->setCachePressureForTest(cachePressureThreshold + 5);

    // Sleep for a time because increaseTargetSnapshotWindowSize() enforces a wait time between
    // updates.
    sleepmillis(2 * minTimeBetweenInc);

    increaseTargetSnapshotWindowSize(_opCtx.get());
    auto snapshotWindowSecondsThree =
        snapshotWindowParams.targetSnapshotHistoryWindowInSeconds.load();

    ASSERT_EQ(snapshotWindowSecondsThree,
              static_cast<int>(snapshotWindowSecondsTwo * windowMultiplicativeDecrease));

    engine->setCachePressureForTest(cachePressureThreshold - 5);

    /**
     * Test that the size cannot be increased above the maximum size.
     */

    // Integers round down, so add 1 to make sure it reaches the max.
    int numIncreasesToReachMax =
        (maxTargetSnapshotWindowSeconds - snapshotWindowSecondsThree) / windowAdditiveIncrease + 1;
    for (int i = 0; i < numIncreasesToReachMax; ++i) {
        sleepmillis(2 * minTimeBetweenInc);
        increaseTargetSnapshotWindowSize(_opCtx.get());
    }

    // Should be at max.
    auto snapshotWindowSecondsFour =
        snapshotWindowParams.targetSnapshotHistoryWindowInSeconds.load();
    ASSERT_EQ(snapshotWindowSecondsFour, maxTargetSnapshotWindowSeconds);

    // An attempt to increase beyond max should have no effect.
    sleepmillis(2 * minTimeBetweenInc);
    increaseTargetSnapshotWindowSize(_opCtx.get());
    auto snapshotWindowSecondsFive =
        snapshotWindowParams.targetSnapshotHistoryWindowInSeconds.load();
    ASSERT_EQ(snapshotWindowSecondsFive, maxTargetSnapshotWindowSeconds);
}

}  // namespace
}  // namespace mongo
