/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include <functional>

#include "mongo/util/tracking_allocator.h"

namespace mongo {

template <class T>
class Tracked {
public:
    Tracked(TrackingAllocatorStats& stats, T obj) : _stats(stats), _obj(std::move(obj)) {
        _stats.get().bytesAllocated.fetchAndAddRelaxed(_obj.allocated());
    }

    Tracked(Tracked&) = delete;
    Tracked(Tracked&& other) : _stats(other._stats), _obj(std::move(other._obj)) {
        invariant(other._obj.allocated() == 0);
    }

    Tracked& operator=(Tracked&) = delete;
    Tracked& operator=(Tracked&& other) {
        if (&other == this) {
            return *this;
        }

        _stats = other._stats;
        _obj = std::move(other._obj);
        invariant(other._obj.allocated() == 0);

        return *this;
    }

    ~Tracked() {
        _stats.get().bytesAllocated.fetchAndSubtractRelaxed(_obj.allocated());
    }

    T& get() {
        return _obj;
    }

    const T& get() const {
        return _obj;
    }

private:
    std::reference_wrapper<TrackingAllocatorStats> _stats;
    T _obj;
};

/**
 * A TrackingContext is a factory style class that constructs TrackingAllocator objects under a
 * single instance of TrackingAllocatorStats and provides access to these stats.
 */
class TrackingContext {
public:
    TrackingContext() = default;
    ~TrackingContext() = default;

    template <class T>
    Tracked<T> makeTracked(T obj) {
        return {_stats, std::move(obj)};
    }

    template <class T>
    TrackingAllocator<T> makeAllocator() {
        return TrackingAllocator<T>(&_stats);
    }

    uint64_t allocated() const {
        return _stats.bytesAllocated.loadRelaxed();
    }

private:
    TrackingAllocatorStats _stats;
};

}  // namespace mongo
