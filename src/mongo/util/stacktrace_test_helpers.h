// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include <functional>
#include <vector>

// Must be a named namespace so the functions we want to unwind through have external linkage.
// Without that, the compiler optimizes them away.
namespace mongo::stacktrace_test {

struct Context {
    // Trickery with std::vector of function pointers is to hide from the optimizer. We use
    // raw function pointers over std::function to ensure we don't see extra frames of
    // std::function::operator() in unoptimized builds.
    std::vector<void (*)(Context&)> plan;
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
