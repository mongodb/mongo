// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/util/timer.h"

#include "mongo/unittest/unittest.h"
#include "mongo/util/tick_source_mock.h"

namespace mongo {
namespace {

TEST(TimerTest, Basic) {
    TickSourceMock<Milliseconds> ts;
    Timer timer(&ts);
    ASSERT_TRUE(timer.isRunning());

    ts.advance(Milliseconds(5000));
    ASSERT_EQ(timer.elapsed(), Milliseconds(5000));
    ASSERT_TRUE(timer.isRunning());
}

TEST(TimerTest, PauseUnpause) {
    TickSourceMock<Milliseconds> ts;
    Timer timer(&ts);
    ASSERT_TRUE(timer.isRunning());

    // Test that once the timer is paused, the elapsed time remains unchanged.
    ts.advance(Milliseconds(5000));
    timer.pause();
    ASSERT_FALSE(timer.isRunning());
    ts.advance(Milliseconds(5000));
    ASSERT_EQ(timer.elapsed(), Milliseconds(5000));

    // Test that once a paused timer is resumed, it resumers tracking time again.
    timer.unpause();
    ASSERT_TRUE(timer.isRunning());
    ts.advance(Milliseconds(5000));
    ASSERT_EQ(timer.elapsed(), Milliseconds(10000));

    // Pause and unpause the timer again and check that the elapsed time still correct.
    timer.pause();
    ASSERT_FALSE(timer.isRunning());
    ts.advance(Milliseconds(5000));
    ASSERT_EQ(timer.elapsed(), Milliseconds(10000));
    timer.unpause();
    ASSERT_TRUE(timer.isRunning());
    ts.advance(Milliseconds(5000));
    ASSERT_EQ(timer.elapsed(), Milliseconds(15000));
}

TEST(TimerTest, Reset) {
    TickSourceMock<Milliseconds> ts;
    Timer timer(&ts);
    ASSERT_TRUE(timer.isRunning());

    // Pause the timer.
    ts.advance(Milliseconds(5000));
    timer.pause();
    ASSERT_FALSE(timer.isRunning());
    ts.advance(Milliseconds(5000));
    ASSERT_EQ(timer.elapsed(), Milliseconds(5000));

    // Test that when a timer is reset, it clears all its internal state and restarts the timer.
    timer.reset();
    ASSERT_TRUE(timer.isRunning());
    ASSERT_EQ(timer.elapsed(), Milliseconds(0));
    ts.advance(Milliseconds(5000));
    ASSERT_EQ(timer.elapsed(), Milliseconds(5000));
}

}  // namespace
}  // namespace mongo
