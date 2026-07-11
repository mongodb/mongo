// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/skip_and_limit.h"

#include "mongo/unittest/unittest.h"

#include <memory>

#include <boost/none.hpp>


namespace mongo {

namespace {

TEST(LimitThenSkip, SkipIsCappedWhenLargerThanLimit) {
    LimitThenSkip source(5, 10);
    ASSERT_EQ(*source.getLimit(), 5);
    ASSERT_EQ(*source.getSkip(), 5);
}

TEST(LimitThenSkip, FlipToSkipThenLimit) {
    LimitThenSkip source(15, 5);
    SkipThenLimit converted = source.flip();
    ASSERT_EQ(*converted.getSkip(), 5);
    ASSERT_EQ(*converted.getLimit(), 10);
}

TEST(LimitThenSkip, FlipOnlyLimit) {
    LimitThenSkip source(15, boost::none);
    SkipThenLimit converted = source.flip();
    ASSERT(!converted.getSkip());
    ASSERT_EQ(*converted.getLimit(), 15);
}

TEST(LimitThenSkip, FlipOnlySkip) {
    LimitThenSkip source(boost::none, 5);
    SkipThenLimit converted = source.flip();
    ASSERT_EQ(*converted.getSkip(), 5);
    ASSERT(!converted.getLimit());
}

}  // namespace
}  // namespace mongo
