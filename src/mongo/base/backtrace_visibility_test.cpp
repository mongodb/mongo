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

#include "backtrace_visibility_test.h"

#include "mongo/platform/compiler.h"
#include "mongo/util/stacktrace.h"

#include <sstream>

// "static" means internal linkage only for functions in global namespace.
NOINLINE_DECL static void static_function(std::string& s) {
    std::ostringstream ostream;
    mongo::printStackTrace(ostream);
    s = ostream.str();
    // Prevent tail-call optimization.
    asm volatile("");  // NOLINT
}

namespace {
NOINLINE_DECL void anonymous_namespace_function(std::string& s) {
    static_function(s);
    // Prevent tail-call optimization.
    asm volatile("");  // NOLINT
}
}  // namespace

NOINLINE_DECL MONGO_COMPILER_API_HIDDEN_FUNCTION void hidden_function(std::string& s) {
    anonymous_namespace_function(s);
    // Prevent tail-call optimization.
    asm volatile("");  // NOLINT
}

namespace mongo {

namespace unwind_test_detail {
NOINLINE_DECL void normal_function(std::string& s) {
    hidden_function(s);
    // Prevent tail-call optimization.
    asm volatile("");  // NOLINT
}
}  // namespace unwind_test_detail
}  // namespace mongo
