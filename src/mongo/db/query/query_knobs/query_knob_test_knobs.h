// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/query_knobs/query_knob.h"
#include "mongo/db/query/query_knobs/query_knob_test_gen.h"

// clang-format off
#define MONGO_EXPAND_QUERY_KNOBS_TEST(KNOB) \
    KNOB(testIntKnob, "testIntKnob", gTestIntKnob, getTestInt) \
    KNOB(testDoubleKnob, "testDoubleKnob", gTestDoubleKnob, getTestDouble) \
    KNOB(testBoolKnob, "testBoolKnob", gTestBoolKnob, getTestBool) \
    KNOB(testLowFcvKnob, "testLowFcvKnob", gTestLowFcvKnob, getTestLowFcv) \
    KNOB(testLLKnob, "testLLKnob", gTestLLKnob, getTestLL) \
    KNOB(testEnumKnob, "testEnumKnob", TestEnumKnob, getTestEnum)
    /**/
// clang-format on

namespace mongo::test_knobs {
DECLARE_QUERY_KNOBS(TestKnobs, MONGO_EXPAND_QUERY_KNOBS_TEST)
}  // namespace mongo::test_knobs
