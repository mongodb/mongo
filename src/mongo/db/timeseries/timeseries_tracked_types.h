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

#include <absl/container/inlined_vector.h>
#include <boost/smart_ptr/allocate_unique.hpp>
#include <memory>
#include <scoped_allocator>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/timeseries/timeseries_tracking_allocator.h"
#include "mongo/db/timeseries/timeseries_tracking_context.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/string_map.h"

namespace mongo::timeseries {

template <class T>
using shared_tracked_ptr = std::shared_ptr<T>;

template <class T, class... Args>
shared_tracked_ptr<T> make_shared_tracked(TrackingContext& trackingContext, Args&&... args) {
    return std::allocate_shared<T>(trackingContext.makeAllocator<T>(), std::forward<Args>(args)...);
}

template <class T>
class unique_tracked_ptr {
public:
    unique_tracked_ptr() = delete;

    template <class... Args>
    unique_tracked_ptr(TrackingContext& trackingContext, Args&&... args)
        : _uniquePtr(boost::allocate_unique<T>(trackingContext.makeAllocator<T>(),
                                               std::forward<Args>(args)...)) {}
    unique_tracked_ptr(unique_tracked_ptr& utp) noexcept : _uniquePtr(*utp.get()){};
    unique_tracked_ptr(unique_tracked_ptr&&) = default;
    ~unique_tracked_ptr() = default;

    T* operator->() {
        return _uniquePtr.get().ptr();
    }

    T* operator->() const {
        return _uniquePtr.get().ptr();
    }

    T* get() {
        return _uniquePtr.get().ptr();
    }

    T* get() const {
        return _uniquePtr.get().ptr();
    }

    T& operator*() {
        return *get();
    }

    T& operator*() const {
        return *get();
    }

private:
    std::unique_ptr<T, boost::alloc_deleter<T, TrackingAllocator<T>>> _uniquePtr;
};

template <class T, class... Args>
unique_tracked_ptr<T> make_unique_tracked(TrackingContext& trackingContext, Args&&... args) {
    return unique_tracked_ptr<T>(trackingContext, std::forward<Args>(args)...);
}


template <class Key, class T, class Compare = std::less<Key>>
using tracked_map =
    std::map<Key,
             T,
             Compare,
             std::scoped_allocator_adaptor<timeseries::TrackingAllocator<std::pair<const Key, T>>>>;

template <class Key, class T, class Compare = std::less<Key>>
tracked_map<Key, T, Compare> make_tracked_map(TrackingContext& trackingContext) {
    return tracked_map<Key, T, Compare>(trackingContext.makeAllocator<T>());
}

template <class Key,
          class Value,
          class Hasher = DefaultHasher<Key>,
          class KeyEqual = std::equal_to<Key>>
using tracked_unordered_map = stdx::unordered_map<
    Key,
    Value,
    Hasher,
    KeyEqual,
    std::scoped_allocator_adaptor<timeseries::TrackingAllocator<std::pair<const Key, Value>>>>;

template <class Key,
          class Value,
          class Hasher = DefaultHasher<Key>,
          class KeyEqual = std::equal_to<Key>>
tracked_unordered_map<Key, Value, Hasher> make_tracked_unordered_map(
    TrackingContext& trackingContext) {
    return tracked_unordered_map<Key, Value, Hasher, KeyEqual>(
        trackingContext.makeAllocator<Value>());
}

using tracked_string =
    std::basic_string<char, std::char_traits<char>, timeseries::TrackingAllocator<char>>;

template <class... Args>
tracked_string make_tracked_string(TrackingContext& trackingContext, Args... args) {
    return tracked_string(args..., trackingContext.makeAllocator<char>());
}

struct TrackedStringMapHasher {
    using is_transparent = void;

    size_t operator()(StringData sd) const {
        return absl::Hash<absl::string_view>{}(absl::string_view{sd.data(), sd.size()});
    }

    size_t operator()(const tracked_string& s) const {
        return operator()(StringData{s.data(), s.size()});
    }
};

struct TrackedStringMapEq {
    using is_transparent = void;

    bool operator()(StringData lhs, StringData rhs) const {
        return lhs == rhs;
    }

    bool operator()(const tracked_string& lhs, StringData rhs) const {
        return StringData{lhs.data(), lhs.size()} == rhs;
    }

    bool operator()(StringData lhs, const tracked_string& rhs) const {
        return lhs == StringData{rhs.data(), rhs.size()};
    }

    bool operator()(const tracked_string& lhs, const tracked_string& rhs) const {
        return lhs == rhs;
    }
};

template <class Value>
using TrackedStringMap =
    absl::flat_hash_map<tracked_string,
                        Value,
                        TrackedStringMapHasher,
                        TrackedStringMapEq,
                        std::scoped_allocator_adaptor<
                            timeseries::TrackingAllocator<std::pair<const tracked_string, Value>>>>;

template <class Value>
TrackedStringMap<Value> makeTrackedStringMap(TrackingContext& trackingContext) {
    return TrackedStringMap<Value>(
        trackingContext
            .makeAllocator<typename TrackedStringMap<Value>::allocator_type::value_type>());
}

template <class T>
using tracked_vector =
    std::vector<T, std::scoped_allocator_adaptor<timeseries::TrackingAllocator<T>>>;

template <class T, class... Args>
tracked_vector<T> make_tracked_vector(TrackingContext& trackingContext, Args... args) {
    return tracked_vector<T>(args..., trackingContext.makeAllocator<T>());
}

template <class T>
using tracked_list = std::list<T, std::scoped_allocator_adaptor<timeseries::TrackingAllocator<T>>>;

template <class T>
tracked_list<T> make_tracked_list(TrackingContext& trackingContext) {
    return tracked_list<T>(trackingContext.makeAllocator<T>());
}

template <class Key>
using tracked_set = std::
    set<Key, std::less<Key>, std::scoped_allocator_adaptor<timeseries::TrackingAllocator<Key>>>;

template <class Key>
tracked_set<Key> make_tracked_set(TrackingContext& trackingContext) {
    return tracked_set<Key>(trackingContext.makeAllocator<Key>());
}

template <class T, std::size_t N>
using tracked_inlined_vector =
    absl::InlinedVector<T, N, std::scoped_allocator_adaptor<timeseries::TrackingAllocator<T>>>;

template <class T, std::size_t N>
tracked_inlined_vector<T, N> make_tracked_inlined_vector(TrackingContext& trackingContext) {
    return tracked_inlined_vector<T, N>(trackingContext.makeAllocator<T>());
}

class TrackableBSONObj {
public:
    explicit TrackableBSONObj(BSONObj obj) : _obj(std::move(obj)) {
        invariant(_obj.isOwned() || _obj.isEmptyPrototype());
    }

    size_t allocated() const {
        return !_obj.isEmptyPrototype() ? _obj.objsize() : 0;
    }

    BSONObj& get() {
        return _obj;
    }

    const BSONObj& get() const {
        return _obj;
    }

private:
    BSONObj _obj;
};
using TrackedBSONObj = Tracked<TrackableBSONObj>;

TrackedBSONObj makeTrackedBson(TrackingContext& trackingContext, BSONObj obj);

}  // namespace mongo::timeseries
