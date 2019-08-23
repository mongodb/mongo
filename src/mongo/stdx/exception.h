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

#pragma once

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

namespace mongo::stdx {

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
}  // namespace mongo::stdx
