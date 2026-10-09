// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/unittest/unittest.h"

#include <utility>
#include <vector>

namespace mongo {

/**
 * Asserts that restoring 'original's serialized state into the fresh instance 'restored' makes it
 * indistinguishable from 'original'. Every WindowFunctionState that implements
 * serializeState()/restoreState() should be covered by a test using this helper.
 */
template <typename State>
void assertSerializeRoundTrip(State& original,
                              State& restored,
                              const std::vector<std::pair<bool /* isAdd */, Value>>& furtherOps) {
    auto assertEqualValues = [&] {
        auto expected = original.getValue();
        auto actual = restored.getValue();
        ASSERT_VALUE_EQ(actual, expected);
        ASSERT_EQUALS(actual.getType(), expected.getType());
    };

    auto state = original.serializeState();
    restored.restoreState(state);
    ASSERT_VALUE_EQ(restored.serializeState(), state);
    assertEqualValues();
    for (const auto& [isAdd, value] : furtherOps) {
        if (isAdd) {
            original.add(value);
            restored.add(value);
        } else {
            original.remove(value);
            restored.remove(value);
        }
        assertEqualValues();
    }
}

}  // namespace mongo
