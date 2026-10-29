// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/wiredtiger/wiredtiger_cursor_helpers.h"

#include "mongo/platform/random.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

// Tests set the ratio via unittest::ServerParameterGuard. A MONGO_INITIALIZER sets the ratio
// at 0.0 at startup for tests in general; these unit tests override that value by the using the
// RAII controller for the duration of each test.

TEST(ChooseBlindWriteOverwriteTest, ProviderDisallowsReturnsDefault) {
    PseudoRandom prng(123);
    unittest::ServerParameterGuard ratio{"wiredTigerBlindWriteRatio", 0.5};

    ASSERT_TRUE(chooseBlindWriteOverwrite(
        /*defaultOverwrite=*/true, /*providerAllowsBlindWrite=*/false, prng));
    // Multiple calls to catch a broken gate that would let the ratio=0.5 sampler run.
    for (int i = 0; i < 100; ++i) {
        ASSERT_FALSE(chooseBlindWriteOverwrite(
            /*defaultOverwrite=*/false, /*providerAllowsBlindWrite=*/false, prng));
    }
}

TEST(ChooseBlindWriteOverwriteTest, RatioOneAlwaysBlind) {
    PseudoRandom prng(123);
    unittest::ServerParameterGuard ratio{"wiredTigerBlindWriteRatio", 1.0};

    for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE(chooseBlindWriteOverwrite(
            /*defaultOverwrite=*/false, /*providerAllowsBlindWrite=*/true, prng));
        ASSERT_TRUE(chooseBlindWriteOverwrite(
            /*defaultOverwrite=*/true, /*providerAllowsBlindWrite=*/true, prng));
    }
}

TEST(ChooseBlindWriteOverwriteTest, RatioZeroNeverUpgrades) {
    PseudoRandom prng(123);
    unittest::ServerParameterGuard ratio{"wiredTigerBlindWriteRatio", 0.0};

    for (int i = 0; i < 100; ++i) {
        // With ratio=0.0 and the provider allowing, a default of false is never upgraded.
        ASSERT_FALSE(chooseBlindWriteOverwrite(
            /*defaultOverwrite=*/false, /*providerAllowsBlindWrite=*/true, prng));
        // defaultOverwrite=true is preserved regardless of ratio: the helper is a one-way upgrade
        // (false -> maybe true) and never downgrades true -> false, because callers passing true
        // have invariants that depend on it.
        ASSERT_TRUE(chooseBlindWriteOverwrite(
            /*defaultOverwrite=*/true, /*providerAllowsBlindWrite=*/true, prng));
    }
}

TEST(ChooseBlindWriteOverwriteTest, IntermediateRatioSamplesRoughly) {
    PseudoRandom prng(123);
    unittest::ServerParameterGuard ratio{"wiredTigerBlindWriteRatio", 0.5};

    constexpr int iter = 2000;
    int blindCount = 0;
    for (int i = 0; i < iter; ++i) {
        if (chooseBlindWriteOverwrite(
                /*defaultOverwrite=*/false, /*providerAllowsBlindWrite=*/true, prng)) {
            ++blindCount;
        }
    }

    // Broad error bars, enough to catch "never fires" or "always fires" regressions without
    // making the test statistically flaky.
    ASSERT_GT(blindCount, 800);
    ASSERT_LT(blindCount, 1200);
}

}  // namespace
}  // namespace mongo
