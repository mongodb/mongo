// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

namespace {

void IntegerEqualsItself(int value) {
    EXPECT_EQ(value, value);
}

FUZZ_TEST(CompiledbFuzzTest, IntegerEqualsItself);

}  // namespace
