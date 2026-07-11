// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/functional.h"
#include "mongo/util/modules.h"

#include <mutex>
#include <vector>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * Offers a type which allows idl to register observers for server parameters at runtime.
 *
 * An easy way to leverage this type is to register it as an on_update handler via
 *   on_update: std::ref(someGlobalMutableObserverRegistry)
 *
 * Then adding observers via someGlobalMutableObserverRegistry.addObserver();
 */
template <typename T>
class MutableObserverRegistry {
public:
    using Argument = T;

    void addObserver(unique_function<void(const T&)> observer) {
        std::lock_guard lk(_mutex);
        _registry.emplace_back(std::move(observer));
    }

    Status operator()(const T& t) {
        std::lock_guard lk(_mutex);
        for (const auto& observer : _registry) {
            observer(t);
        }

        return Status::OK();
    }

private:
    std::mutex _mutex;
    std::vector<unique_function<void(const T&)>> _registry;
};

}  // namespace mongo
