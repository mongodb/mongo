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

#include "mongo/db/storage/wiredtiger/wiredtiger_cursor_helpers.h"

#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/platform/random.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

// Tests set the ratio via RAIIServerParameterControllerForTest. A MONGO_INITIALIZER sets the ratio
// at 0.0 at startup for tests in general; these unit tests override that value by the using the
// RAII controller for the duration of each test.

TEST(ChooseBlindWriteOverwriteTest, ProviderDisallowsReturnsDefault) {
    PseudoRandom prng(123);
    RAIIServerParameterControllerForTest ratio{"wiredTigerBlindWriteRatio", 0.5};

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
    RAIIServerParameterControllerForTest ratio{"wiredTigerBlindWriteRatio", 1.0};

    for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE(chooseBlindWriteOverwrite(
            /*defaultOverwrite=*/false, /*providerAllowsBlindWrite=*/true, prng));
        ASSERT_TRUE(chooseBlindWriteOverwrite(
            /*defaultOverwrite=*/true, /*providerAllowsBlindWrite=*/true, prng));
    }
}

TEST(ChooseBlindWriteOverwriteTest, RatioZeroNeverUpgrades) {
    PseudoRandom prng(123);
    RAIIServerParameterControllerForTest ratio{"wiredTigerBlindWriteRatio", 0.0};

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
    RAIIServerParameterControllerForTest ratio{"wiredTigerBlindWriteRatio", 0.5};

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
