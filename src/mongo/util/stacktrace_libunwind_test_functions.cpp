// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * This source file and its header are for testing that static, hidden, etc.
 * functions appear in backtraces, see unwind_test.cpp.
 */

#include "mongo/util/stacktrace_libunwind_test_functions.h"

#include "mongo/platform/compiler.h"
#include "mongo/util/stacktrace.h"

#define PREVENT_TAIL_CALL asm volatile("")  // NOLINT

// "static" means internal linkage only for functions at namespace scope.
MONGO_COMPILER_NOINLINE static void staticFunction(std::ostream& s) {
    mongo::printStackTrace(s);
    PREVENT_TAIL_CALL;
}

namespace {
MONGO_COMPILER_NOINLINE void anonymousNamespaceFunction(std::ostream& s) {
    staticFunction(s);
    PREVENT_TAIL_CALL;
}
}  // namespace

MONGO_COMPILER_NOINLINE MONGO_COMPILER_API_HIDDEN_FUNCTION void hiddenFunction(std::ostream& s) {
    anonymousNamespaceFunction(s);
    PREVENT_TAIL_CALL;
}

namespace mongo {

namespace unwind_test_detail {
MONGO_COMPILER_NOINLINE void normalFunction(std::ostream& s) {
    hiddenFunction(s);
    PREVENT_TAIL_CALL;
}
}  // namespace unwind_test_detail
}  // namespace mongo
