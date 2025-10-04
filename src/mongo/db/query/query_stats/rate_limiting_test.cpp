/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/query/query_stats/rate_limiting.h"

#include "mongo/unittest/unittest.h"

namespace mongo {

TEST(RateLimiterTest, SlidingWindowSucceeds) {
    auto rl = RateLimiter();
    rl.configureWindowBased(1);
    ASSERT_TRUE(rl.handle());
}

TEST(RateLimiterTest, SlidingWindowFails) {
    auto rl = RateLimiter();
    rl.configureWindowBased(0);
    ASSERT_FALSE(rl.handle());
}

TEST(RateLimiterTest, SlidingWindowSucceedsThenFails) {
    auto rl = RateLimiter();
    rl.configureWindowBased(1);
    ASSERT_TRUE(rl.handle());
    ASSERT_FALSE(rl.handle());
    ASSERT_FALSE(rl.handle());
}

TEST(RateLimiterTest, SampleBasedSucceeds) {
    auto rl = RateLimiter();
    rl.configureSampleBased(10 /* 1% */, 0 /* seed */);
    size_t requestCount = 10000;
    size_t numHandles = 0;
    for (size_t i = 0; i < requestCount; i++) {
        if (rl.handle()) {
            numHandles++;
        }
    }
    ASSERT_APPROX_EQUAL((double)numHandles, 100, 5 /* variance for randomness */);
}

TEST(RateLimiterTest, SampleBasedFails) {
    auto rl = RateLimiter();
    rl.configureSampleBased(0 /* 0% */, 0 /* seed */);
    ASSERT_FALSE(rl.handle());
}

TEST(RateLimiterTest, PolicyGetter) {
    {
        auto rl = RateLimiter();
        rl.configureWindowBased(100);
        ASSERT_EQ(rl.getSamplingRate(), 100);
        ASSERT_TRUE(rl.getPolicyType() == RateLimiter::PolicyType::kWindowBasedPolicy);
    }
    {
        auto rl = RateLimiter();
        rl.configureSampleBased(10 /* 1% */, 0 /* seed */);
        ASSERT_EQ(rl.getSamplingRate(), 10);
        ASSERT_TRUE(rl.getPolicyType() == RateLimiter::PolicyType::kSampleBasedPolicy);
    }
}

}  // namespace mongo
