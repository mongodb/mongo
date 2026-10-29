// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <concepts>

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace clang_checked {

template <typename T>
concept BaseLockable = requires(T obj) {
    { obj.lock() };
    { obj.unlock() };
};

template <typename T>
concept TryLockable = requires(T obj) {
    requires BaseLockable<T>;
    { obj.try_lock() };
};

// A mutex that publishes an observation token, i.e. an ObservableMutex.
template <typename T>
concept Observable = requires(const T obj) {
    { obj.token() };
};

}  // namespace clang_checked
}  // namespace mongo
