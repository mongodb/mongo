// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include <folly/TokenBucket.h>

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

/**
 * Simple test to ensure the folly::TokenBucket library has been vendored in properly.
 */
TEST(TokenBucketTest, CanConsume) {
    folly::DynamicTokenBucket tokenBucket;
    ASSERT_TRUE(tokenBucket.consume(1.0, 1.0, 20.0));
}

}  // namespace
}  // namespace mongo
