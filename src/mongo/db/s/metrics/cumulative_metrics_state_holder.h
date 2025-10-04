/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/platform/atomic_word.h"
#include "mongo/util/functional.h"

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

    std::array<AtomicWord<int64_t>, Size>* getStateArrayFor() {
        return &_roleStateList;
    }

    template <typename T>
    const AtomicWord<int64_t>* getStateCounter(T state) const {
        auto index = static_cast<size_t>(state);
        invariant(index >= 0);
        invariant(index < Size);
        return &_roleStateList[index];
    }

    template <typename T>
    AtomicWord<int64_t>* getMutableStateCounter(T state) {
        auto index = static_cast<size_t>(state);
        invariant(index >= 0);
        invariant(index < Size);
        return &_roleStateList[index];
    }


private:
    std::array<AtomicWord<int64_t>, Size> _roleStateList;
};
}  // namespace mongo
