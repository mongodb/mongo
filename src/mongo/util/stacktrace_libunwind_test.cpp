/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include "mongo/base/string_data.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/stacktrace_libunwind_test_functions.h"
#include "mongo/util/stacktrace_test_helpers.h"

#include <cstdlib>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include <cxxabi.h>
#include <libunwind.h>  // IWYU pragma: keep

#include <fmt/format.h>
#include <fmt/printf.h>  // IWYU pragma: keep

// IWYU pragma: no_include "libunwind-x86_64.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#if defined(__s390__) || defined(__s390x__) || defined(__zarch__)
#define MONGO_S390 1
#endif

namespace mongo {

// Must be a named namespace so the functions we want to unwind through have external linkage.
// Without that, the compiler optimizes them away.
namespace unwind_test_detail {

std::string trace() {
    std::string out;
    unw_cursor_t cursor;
    unw_context_t context;

    // Initialize cursor to current frame for local unwinding.
    unw_getcontext(&context);
    unw_init_local(&cursor, &context);
    // Unwind frames one by one, going up the frame stack.
    while (unw_step(&cursor) > 0) {
        unw_word_t offset, pc;
        unw_get_reg(&cursor, UNW_REG_IP, &pc);
        if (pc == 0) {
            break;
        }
        out += fmt::format("0x{:x}:", pc);
        char sym[32 << 10];
        char* name = sym;
        int err;
        if ((err = unw_get_proc_name(&cursor, sym, sizeof(sym), &offset)) != 0) {
            out +=
                fmt::format(" -- error: unable to obtain symbol name for this frame: {:d}\n", err);
            continue;
        }
        name = sym;
        int status;
        char* demangled_name;
        if ((demangled_name = abi::__cxa_demangle(sym, nullptr, nullptr, &status))) {
            name = demangled_name;
        }
        out += fmt::format(" ({:s}+0x{:x})\n", name, offset);
        if (name != sym) {
            free(name);  // allocated by abi::__cxa_demangle
        }
    }
    return out;
}

void assertAndRemovePrefix(StringData& v, StringData prefix) {
    auto pos = v.find(prefix);
    ASSERT(pos != v.npos) << fmt::format("expected to find '{}' in '{}'", prefix, v);
    v.remove_prefix(pos + prefix.size());
}

void assertAndRemoveSuffix(StringData& v, StringData suffix) {
    auto pos = v.rfind(suffix);
    ASSERT(pos != v.npos) << fmt::format("expected to find '{}' in '{}'", suffix, v);
    v.remove_suffix(v.size() - pos);
}

// On our current version of libunwind, unw_get_proc_name() is failing
// consistently on s390. This isn't a problem for our production stack
// traces, as falling back to dladdr() works. It does, however, break
// these libunwind-specific tests.
#ifndef MONGO_S390

TEST(Unwind, Demangled) {
    std::string stacktrace;
    stacktrace_test::executeUnderCallstack<6>([&] { stacktrace = trace(); });

    // Check that these function names appear in the trace, in order.
    // There will of course be characters between them but ignore that.
    const std::string frames[] = {
        "void mongo::stacktrace_test::callNext<0>(mongo::stacktrace_test::Context&)",
        "void mongo::stacktrace_test::callNext<1>(mongo::stacktrace_test::Context&)",
        "void mongo::stacktrace_test::callNext<2>(mongo::stacktrace_test::Context&)",
        "void mongo::stacktrace_test::callNext<3>(mongo::stacktrace_test::Context&)",
        "void mongo::stacktrace_test::callNext<4>(mongo::stacktrace_test::Context&)",
        "void mongo::stacktrace_test::callNext<5>(mongo::stacktrace_test::Context&)",
        "main",
    };
    size_t pos = 0;
    for (const auto& expected : frames) {
        pos = stacktrace.find(expected, pos);
        ASSERT_NE(pos, stacktrace.npos) << stacktrace;
    }
}

TEST(Unwind, Linkage) {
    // From backtrace_visibility_test.h. Calls a chain of functions and stores the backtrace at the
    // bottom in the "stacktrace" argument.
    std::ostringstream os;
    normalFunction(os);
    std::string stacktrace = os.str();

    StringData view = stacktrace;

    LOGV2_OPTIONS(31429, {logv2::LogTruncation::Disabled}, "Trace", "trace"_attr = view);

    // Remove the backtrace JSON object, which is all one line.
    assertAndRemovePrefix(view, R"(BACKTRACE: {"backtrace":)");
    assertAndRemovePrefix(view, "}\n");

    StringData remainder = stacktrace;

    // Check that these function names appear in the trace, in order. The tracing code which
    // preceded our libunwind integration could *not* symbolize hiddenFunction,
    // anonymousNamespaceFunction, or staticFunction.
    //
    // When libunwind does cursor stepping, it picks up these procedure names with
    // `unw_get_proc_name`. However, it doesn't provide a way to convert raw addrs to procedure
    // names otherwise, so we can't do an `unw_backtrace` and then lookup the names in the usual
    // way (i.e. `mergeDlInfo` calling `dladdr`).
    std::string frames[] = {
        "printStackTrace",
        "staticFunction",
        "anonymousNamespaceFunction",
        "hiddenFunction",
        "normalFunction",
    };

    for (const auto& name : frames) {
        auto pos = remainder.find(name);
        if (pos == remainder.npos) {
            LOGV2_OPTIONS(
                31378, {logv2::LogTruncation::Disabled}, "BACKTRACE", "trace"_attr = view);
            FAIL(fmt::format("name '{}' is missing or out of order in sample backtrace", name));
        }
        LOGV2(31379, "Removing prefix", "prefix"_attr = remainder.substr(0, pos));
        remainder.remove_prefix(pos);
    }
}

#endif  // MONGO_S390

}  // namespace unwind_test_detail
}  // namespace mongo
