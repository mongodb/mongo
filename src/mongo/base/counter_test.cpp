// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/counter.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
TEST(CounterTest, IncrementDecrement) {
    Counter64 c;
    ASSERT_EQUALS(c.get(), 0);
    c.increment();
    ASSERT_EQUALS(c.get(), 1);
    c.decrement();
    ASSERT_EQUALS(c.get(), 0);
    c.decrement(3);
    ASSERT_EQUALS(c.get(), -3);
    c.increment(1);
    ASSERT_EQUALS(c.get(), -2);
    c.decrement(-1);
    ASSERT_EQUALS(c.get(), -1);
}

TEST(CounterTest, IncrementDecrementRelaxed) {
    Counter64 c;
    ASSERT_EQUALS(c.get(), 0);
    c.incrementRelaxed();
    ASSERT_EQUALS(c.get(), 1);
    c.decrementRelaxed();
    ASSERT_EQUALS(c.get(), 0);
    c.decrementRelaxed(3);
    ASSERT_EQUALS(c.get(), -3);
    c.incrementRelaxed(1);
    ASSERT_EQUALS(c.get(), -2);
    c.decrementRelaxed(-1);
    ASSERT_EQUALS(c.get(), -1);
}

}  // namespace
}  // namespace mongo
