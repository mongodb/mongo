/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/platform/throw_hook.h"

namespace mongo {

namespace {

ThrowHook& _getThrowHook() {
    static auto& h = *new ThrowHook();
    return h;
}

}  // namespace

const ThrowHook& getThrowHook() {
    return _getThrowHook();
}

void setThrowHook(ThrowHook hook) {
    _getThrowHook() = std::move(hook);
}

}  // namespace mongo

extern "C" {

// This is linker black magic.
//
// The --wrap==__cxa_throw option causes all __cxa_throw calls in our
// codebase to be linked to __wrap___cxa_throw instead. We call our
// hook, then call the standard library __cxa_throw via the symbol
// __real___cxa_throw (more --wrap magic).

[[noreturn]] void __real___cxa_throw(void* ex, std::type_info* tinfo, void (*dest)(void*));

[[noreturn]] void __wrap___cxa_throw(void* ex, std::type_info* tinfo, void (*dest)(void*)) {
    // protect against recursion when logging inside throw
    thread_local bool inThrow = false;
    const auto& hook = mongo::getThrowHook();

    if (hook && !inThrow) {
        inThrow = true;
        try {
            hook(tinfo, ex);
        } catch (...) {
            // The hook call is best effort, so discard any exception.
            // We're going to throw the real exception immediately
            // below here.
        }
        inThrow = false;
    }

    __real___cxa_throw(ex, tinfo, dest);
}

}  // extern "C"
