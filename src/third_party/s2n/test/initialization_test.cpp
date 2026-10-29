// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/unittest/unittest.h"

#include <s2n.h>

namespace mongo {
namespace {

/**
 * Verify that we can initialize the s2n library from third-party code.
 */
TEST(s2nCompilationTest, InitSucceeds) {
    ASSERT(s2n_init() == 0);
}

}  // namespace
}  // namespace mongo
