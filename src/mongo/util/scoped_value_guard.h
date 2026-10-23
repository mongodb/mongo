// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <concepts>
#include <utility>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/** Scope guard to restore an object value. */
template <std::movable T>
class ScopedValueRestore {
public:
    explicit ScopedValueRestore(T& target) : _target{target}, _saved{target} {}

    ScopedValueRestore(ScopedValueRestore&&) = delete;

    ~ScopedValueRestore() {
        _target = std::move(_saved);
    }

private:
    T& _target;
    T _saved;
};

/** Scope guard to set and restore an object value. */
template <std::movable T>
class ScopedValueOverride {
public:
    ScopedValueOverride(T& target, T v)
        : _target{target}, _saved{std::exchange(_target, std::move(v))} {}
    ~ScopedValueOverride() {
        _target = std::move(_saved);
    }

    ScopedValueOverride(ScopedValueOverride&&) = delete;

private:
    T& _target;
    T _saved;
};

}  // namespace mongo
