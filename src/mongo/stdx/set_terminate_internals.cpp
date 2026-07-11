// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#if defined(_WIN32)

#include "mongo/stdx/exception.h"

#include <atomic>
#include <utility>

namespace mongo {
namespace stdx {
// `dispatch_impl` is circularly dependent with the initialization of `terminationHandler`, but
// should not have linkage.  To facilitate matching the definition to the declaration, we make this
// function `static`, rather than placing it in the anonymous namespace.
[[noreturn]] static void dispatch_impl() noexcept;

namespace {
void uninitializedTerminateHandler() {}

::std::atomic<terminate_handler> terminationHandler(&uninitializedTerminateHandler);  // NOLINT

void registerTerminationHook() noexcept {
    const auto oldHandler =
        terminationHandler.exchange(::std::set_terminate(&dispatch_impl));  // NOLINT
    if (oldHandler != &uninitializedTerminateHandler)
        std::abort();  // Someone set the handler, somehow before we got to initialize ourselves.
}


[[maybe_unused]] const int initializeTerminationHandler = []() noexcept {
    registerTerminationHook();
    return 0;
}();

}  // namespace
}  // namespace stdx

stdx::terminate_handler stdx::set_terminate(const stdx::terminate_handler handler) noexcept {
    const auto oldHandler = terminationHandler.exchange(handler);
    if (oldHandler == &uninitializedTerminateHandler)
        std::abort();  // Do not let people set terminate before the initializer has run.
    return oldHandler;
}

stdx::terminate_handler stdx::get_terminate() noexcept {
    const auto currentHandler = terminationHandler.load();
    if (currentHandler == &uninitializedTerminateHandler)
        std::abort();  // Do not let people see the terminate handler before the initializer has
                       // run.
    return currentHandler;
}

void stdx::dispatch_impl() noexcept {
    if (const ::std::terminate_handler handler = terminationHandler.load())
        handler();

    // The standard says that returning from your handler is undefined.  We may as well make the
    // wrapper have stronger guarantees.
    std::abort();
}

void stdx::TerminateHandlerDetailsInterface::dispatch() noexcept {
    return stdx::dispatch_impl();
}
}  // namespace mongo
#endif
