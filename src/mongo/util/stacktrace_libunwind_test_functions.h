// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

/**
 * This header and its source file are for testing that static, hidden, etc.
 * functions appear in backtraces, see unwind_test.cpp.
 */

#include <ostream>

namespace mongo::unwind_test_detail {

// Generates a stack trace by calling functions of various visibility and linkage
// Writes a stack trace to `s` from the deep call stack.
void normalFunction(std::ostream& s);

}  // namespace mongo::unwind_test_detail
