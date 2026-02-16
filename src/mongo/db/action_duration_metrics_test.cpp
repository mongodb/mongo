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

#include "mongo/unittest/unittest.h"
#include "mongo/util/tick_source_mock.h"


namespace mongo {
namespace {

TEST(ActionDurationMetricsTest, ActionDurationTimer) {
    TickSourceMock<Milliseconds> tickSource;
    Milliseconds counter = Milliseconds(0);
    ActionDurationTimer<Milliseconds>::Callback callback = [&](Milliseconds duration) {
        counter += duration;
    };

    {
        ActionDurationTimer<Milliseconds> actionTimer(&tickSource, callback);
        tickSource.advance(Milliseconds(10));

        // Metrics are only advanced when the RAII type is destructed.
        ASSERT_EQ(counter, Milliseconds(0));
    }

    ASSERT_EQ(counter, Milliseconds(10));

    // Advancing time while there's no RAII type in scope has no effect.
    tickSource.advance(Milliseconds(1000));
    ASSERT_EQ(counter, Milliseconds(10));

    {
        ActionDurationTimer<Milliseconds> actionTimer(&tickSource, callback);
        tickSource.advance(Milliseconds(25));
        ASSERT_EQ(counter, Milliseconds(10));
    }

    ASSERT_EQ(counter, Milliseconds(35));

    {
        // Dismissing doesn't execute the callback.
        ActionDurationTimer<Milliseconds> actionTimer(&tickSource, callback);
        tickSource.advance(Milliseconds(50));
        ASSERT_EQ(counter, Milliseconds(35));
        actionTimer.dismiss();
    }

    ASSERT_EQ(counter, Milliseconds(35));
}

}  // namespace
}  // namespace mongo
