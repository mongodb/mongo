// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
        EXPECT_EQ(counter, Milliseconds(0));
    }

    EXPECT_EQ(counter, Milliseconds(10));

    // Advancing time while there's no RAII type in scope has no effect.
    tickSource.advance(Milliseconds(1000));
    EXPECT_EQ(counter, Milliseconds(10));

    {
        ActionDurationTimer<Milliseconds> actionTimer(&tickSource, callback);
        tickSource.advance(Milliseconds(25));
        EXPECT_EQ(counter, Milliseconds(10));
    }

    EXPECT_EQ(counter, Milliseconds(35));

    {
        // Dismissing doesn't execute the callback.
        ActionDurationTimer<Milliseconds> actionTimer(&tickSource, callback);
        tickSource.advance(Milliseconds(50));
        EXPECT_EQ(counter, Milliseconds(35));
        actionTimer.dismiss();
    }

    EXPECT_EQ(counter, Milliseconds(35));
}

}  // namespace
}  // namespace mongo
