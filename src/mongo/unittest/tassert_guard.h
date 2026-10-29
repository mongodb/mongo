// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/unittest/assert.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/testing_proctor.h"

#include <fmt/format.h>

[[MONGO_MOD_PUBLIC]];

namespace mongo::unittest {

/**
 * Verifies that the number of tasserts triggered in scope matches `expected`, and excuses
 * them so the test binary doesn't crash.
 */
class TassertGuard {
public:
    TassertGuard() = default;
    explicit TassertGuard(int expected) : _expected(expected) {}

    TassertGuard(const TassertGuard&) = delete;
    TassertGuard& operator=(const TassertGuard&) = delete;

    ~TassertGuard() {
        auto observed = assertionCount.tripwire.load() - _start;
        if (observed != _expected) {
            FAIL(fmt::format(
                "Unexpected tasserts triggered. observed={}, expected={}", observed, _expected));
        }

        TestingProctor::instance().excuseTripwires(observed);
    }

private:
    int _expected = 1;
    int _start = assertionCount.tripwire.load();
};

/**
 * Assert that `expression` triggers a tassert with `expectedCode` once. An optional extra
 * integer argument may be passed to specify how many times tasserts are expected to trigger.
 */
#define ASSERT_TASSERT_CODE(expression, expectedCode, ...)         \
    do {                                                           \
        ::mongo::unittest::TassertGuard tassertGuard{__VA_ARGS__}; \
        ASSERT_THROWS_CODE(([&] {                                  \
                               (void)(expression);                 \
                           }()),                                   \
                           ::mongo::AssertionException,            \
                           expectedCode);                          \
    } while (false)

}  // namespace mongo::unittest
