// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/progress_meter.h"

#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;

// Trivial unit test to validate build dependencies.
TEST(ProgressMeterTest, ToString) {
    ASSERT_FALSE(ProgressMeter(1).toString().empty());
}

}  // namespace
