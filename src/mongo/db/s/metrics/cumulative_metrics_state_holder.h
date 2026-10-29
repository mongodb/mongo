// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/atomic.h"
#include "mongo/util/functional.h"
#include "mongo/util/modules.h"

#include <algorithm>

#include <boost/optional.hpp>

namespace mongo {
template <typename Role, size_t Size>
class CumulativeMetricsStateHolder {
public:
    CumulativeMetricsStateHolder() {
        for (auto& element : _roleStateList) {
            element.store(0);
        }
    }
    /**
     * The before can be boost::none to represent the initial state transition and
     * after can be boost::none to represent cases where it is no longer active.
     */
    template <typename T>
    void onStateTransition(boost::optional<T> before, boost::optional<T> after) {
        if (before) {
            if (auto counter = getMutableStateCounter(*before)) {
                counter->fetchAndSubtract(1);
            }
        }

        if (after) {
            if (auto counter = getMutableStateCounter(*after)) {
                counter->fetchAndAdd(1);
            }
        }
    }

    std::array<Atomic<int64_t>, Size>* getStateArrayFor() {
        return &_roleStateList;
    }

    template <typename T>
    const Atomic<int64_t>* getStateCounter(T state) const {
        auto index = static_cast<size_t>(state);
        invariant(index >= 0);
        invariant(index < Size);
        return &_roleStateList[index];
    }

    template <typename T>
    Atomic<int64_t>* getMutableStateCounter(T state) {
        auto index = static_cast<size_t>(state);
        invariant(index >= 0);
        invariant(index < Size);
        return &_roleStateList[index];
    }


private:
    std::array<Atomic<int64_t>, Size> _roleStateList;
};
}  // namespace mongo
