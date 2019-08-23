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


#include "mongo/stdx/exception.h"

#include <atomic>
#include <utility>

#if defined(_WIN32)

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
}
();

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
