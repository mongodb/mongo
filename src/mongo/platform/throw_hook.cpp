// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
