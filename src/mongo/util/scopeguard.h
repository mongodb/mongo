// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/compiler.h"
#include "mongo/util/modules.h"

#include <type_traits>
#include <utility>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * Stores a callback to be invoked upon destruction.
 *
 * `F` is normally deduced from the initializer.
 * Examples:
 *
 *     ScopeGuard cleanup([&] { ... });
 *     auto cleanup = ScopeGuard([&] { ... });
 *     ScopeGuard cleanup = [&] { ... };
 *
 * The callback is invoked in a destructor, an implicitly `noexcept` context. As
 * a result, any exceptions escaping the callback are process-fatal.
 */
template <typename F>
class [[nodiscard]] ScopeGuard {
public:
    template <typename FuncArg>
    ScopeGuard(FuncArg&& f) : _func(std::forward<FuncArg>(f)) {}

    // Remove all move and copy, MCE (mandatory copy elision) covers us here.
    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard(ScopeGuard&&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;
    ScopeGuard& operator=(ScopeGuard&&) = delete;

    ~ScopeGuard() noexcept {
        if (!_dismissed)
            _func();
    }

    /** A dismissed ScopeGuard does not invoke its callback. */
    void dismiss() noexcept {
        _dismissed = true;
    }

private:
    F _func;
    bool _dismissed = false;
};

template <typename F>
ScopeGuard(F&&) -> ScopeGuard<std::decay_t<F>>;

}  // namespace mongo

#define MONGO_SCOPEGUARD_CAT2(s1, s2) s1##s2
#define MONGO_SCOPEGUARD_CAT(s1, s2) MONGO_SCOPEGUARD_CAT2(s1, s2)
#define MONGO_SCOPEGUARD_ANON(str) MONGO_SCOPEGUARD_CAT(str, __LINE__)

/**
 * Declares a ScopeGuard having a variable name based on line number.
 * Example:
 *     ON_BLOCK_EXIT([&] { ... });
 *
 * The callback is invoked in a destructor, an implicitly `noexcept` context. As
 * a result, any exceptions escaping the callback are process-fatal.
 */
#define ON_BLOCK_EXIT ScopeGuard MONGO_SCOPEGUARD_ANON(onBlockExit)
