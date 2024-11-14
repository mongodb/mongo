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

#include <boost/smart_ptr/allocate_unique.hpp>
#include <memory>
#include <scoped_allocator>

#include "mongo/util/tracking/allocator.h"
#include "mongo/util/tracking/context.h"

namespace mongo::tracking {

template <class T>
using shared_ptr = std::shared_ptr<T>;

template <class T, class... Args>
shared_ptr<T> make_shared(Context& Context, Args&&... args) {
    return std::allocate_shared<T>(Context.makeAllocator<T>(), std::forward<Args>(args)...);
}

template <class T>
class unique_ptr {
public:
    unique_ptr() = delete;

    template <class... Args>
    unique_ptr(Context& Context, Args&&... args)
        : _uniquePtr(
              boost::allocate_unique<T>(Context.makeAllocator<T>(), std::forward<Args>(args)...)) {}
    unique_ptr(unique_ptr& utp) noexcept : _uniquePtr(*utp.get()){};
    unique_ptr(unique_ptr&&) = default;
    ~unique_ptr() = default;

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

    T* release() noexcept {
        return _uniquePtr.release();
    }

    void reset(T* ptr = nullptr) noexcept {
        _uniquePtr.reset(ptr);
    }

    void swap(const unique_ptr<T>& other) noexcept {
        _uniquePtr.swap(other._uniquePtr);
    }

    bool operator==(const unique_ptr&) const = default;
    auto operator<=>(const unique_ptr&) const = default;

    explicit operator bool() const noexcept {
        return static_cast<bool>(_uniquePtr.get());
    }

    unique_ptr<T>& operator=(unique_ptr<T>&& other) noexcept {
        _uniquePtr = other._uniquePtr;
        return *this;
    }

private:
    std::unique_ptr<T, boost::alloc_deleter<T, Allocator<T>>> _uniquePtr;
};

template <class T, class... Args>
unique_ptr<T> make_unique(Context& Context, Args&&... args) {
    return unique_ptr<T>(Context, std::forward<Args>(args)...);
}

}  // namespace mongo::tracking
