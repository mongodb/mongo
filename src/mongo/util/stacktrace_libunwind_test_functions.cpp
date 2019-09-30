/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

/**
 * This source file and its header are for testing that static, hidden, etc.
 * functions appear in backtraces, see unwind_test.cpp.
 */

#include "stacktrace_libunwind_test_functions.h"

#include "mongo/platform/compiler.h"
#include "mongo/util/stacktrace.h"

#include <sstream>

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
