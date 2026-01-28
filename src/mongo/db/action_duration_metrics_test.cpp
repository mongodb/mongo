/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/action_duration_metrics.h"

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/util/clock_source_mock.h"


namespace mongo {
namespace {

class ActionDurationMetricsTest : public ClockSourceMockServiceContextTest {
public:
    void advanceTime(Milliseconds m) {
        static_cast<ClockSourceMock*>(getServiceContext()->getFastClockSource())->advance(m);
    }

    void assertCounts(const ActionDurationMetrics& metrics,
                      const std::string& action,
                      Milliseconds lastDuration,
                      Milliseconds totalDuration,
                      int64_t count) {
        BSONObjBuilder builder;
        metrics.report(&builder);

        BSONObj obj = builder.obj();
        BSONObj actionObj = obj.getObjectField(action);

        ASSERT_EQ(lastDuration.count(), actionObj.getField("lastDurationMillis").numberLong());
        ASSERT_EQ(totalDuration.count(), actionObj.getField("totalDurationMillis").numberLong());
        ASSERT_EQ(count, actionObj.getField("count").numberLong());
    }
};

TEST_F(ActionDurationMetricsTest, ActionDurationTimer) {
    auto opCtx = makeOperationContext();
    const ActionDurationMetrics& metrics =
        ActionDurationMetrics::getDecoration(opCtx->getServiceContext());
    const std::string action = "TimerTest";

    {
        ActionDurationTimer actionTimer(opCtx.get(), action);
        advanceTime(Milliseconds(10));

        // Metrics are only advanced when the RAII type is destructed.
        assertCounts(metrics, action, Milliseconds(0), Milliseconds(0), 0);
    }

    assertCounts(metrics, action, Milliseconds(10), Milliseconds(10), 1);

    // Advancing time while there's no RAII type in scope has no effect.
    advanceTime(Milliseconds(1000));
    assertCounts(metrics, action, Milliseconds(10), Milliseconds(10), 1);

    {
        ActionDurationTimer actionTimer(opCtx.get(), action);
        advanceTime(Milliseconds(25));
        assertCounts(metrics, action, Milliseconds(10), Milliseconds(10), 1);
    }

    assertCounts(metrics, action, Milliseconds(25), Milliseconds(35), 2);
}

}  // namespace
}  // namespace mongo
