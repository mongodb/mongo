// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <atomic>
#include <exception>
#include <utility>

// This file provides a wrapper over the function registered by `std::set_terminate`.  This
// facilitates making `stdx::set_terminate` work correctly on windows.  In windows,
// `std::set_terminate` works on a per-thread basis.  Our `stdx::thread` header registers our
// handler using the `stdx::terminate_detail::TerminateHandlerInterface::dispatch` as an entry point
// for `std::set_terminate` when a thread starts on windows.  `stdx::set_terminate` sets the handler
// globally for all threads.  Our wrapper, which is registered with each thread, calls the global
// handler.
//
// NOTE: Our wrapper is not initialization order safe.  It is not safe to set the terminate handler
// until main has started.

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace stdx {

// In order to grant `mongo::stdx::thread` access to the dispatch method, we need to know this
// class's name.  A forward-decl header would be overkill for this singular special case.
class thread;

// This must be the same as the definition in standard.  Do not alter this alias.
using ::std::terminate_handler;

#if defined(_WIN32)
class TerminateHandlerDetailsInterface {
    friend ::mongo::stdx::thread;
    static void dispatch() noexcept;
};

terminate_handler set_terminate(const terminate_handler handler) noexcept;

terminate_handler get_terminate() noexcept;

#else
using ::std::get_terminate;  // NOLINT
using ::std::set_terminate;  // NOLINT
#endif
}  // namespace stdx
}  // namespace mongo
