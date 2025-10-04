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

#pragma once

#include "mongo/platform/compiler.h"

#include <type_traits>
#include <utility>

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
