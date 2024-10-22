/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#pragma once

#include <functional>
#include <vector>

// Must be a named namespace so the functions we want to unwind through have external linkage.
// Without that, the compiler optimizes them away.
namespace mongo::stacktrace_test {

struct Context {
    // Trickery with std::vector<std::function> is to hide from the optimizer.
    std::vector<std::function<void(Context&)>> plan;
    std::function<void()> callback;
};

template <int N>
void callNext(Context& ctx) {
    if constexpr (N == 0) {
        ctx.callback();
    } else {
        ctx.plan[N - 1](ctx);
    }

#ifndef _WIN32
    // MSVC doesn't support inline assembly on many architectures including x86-64 and aarch64.
    // So we can't reliably prevent tail call optimization.
    asm volatile("");  // NOLINT
#endif                 // _WIN32
}

template <int N>
Context makeContext() {
    if constexpr (N >= 0) {
        Context ctx = makeContext<N - 1>();
        ctx.plan.emplace_back(callNext<N>);
        return ctx;
    }
    return {};
}

/**
 * Runs the given callback beneath a callstack that contains N frames worth of calls to
 * `stacktrace_test::callNext`, even in the presence of compiler optimizations that could
 * normally optimize away such calls (at least on non-Windows platforms).
 */
template <int N>
void executeUnderCallstack(std::function<void()> callback) {
    auto ctx = stacktrace_test::makeContext<N - 1>();
    ctx.callback = std::move(callback);
    ctx.plan.back()(ctx);
}

}  // namespace mongo::stacktrace_test
