// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"
#include "mongo/util/tracking/allocator.h"
#include "mongo/util/tracking/context.h"

#include <memory>

#include <boost/smart_ptr/allocate_unique.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace tracking {

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
    unique_ptr(Context& Context, std::nullptr_t)
        : _uniquePtr(nullptr, boost::alloc_deleter<T, Allocator<T>>(Context.makeAllocator<T>())) {}
    unique_ptr(unique_ptr&&) = default;
    ~unique_ptr() = default;

    T* operator->() noexcept {
        return _uniquePtr.get().ptr();
    }

    T* operator->() const noexcept {
        return _uniquePtr.get().ptr();
    }

    T* get() noexcept {
        return _uniquePtr.get().ptr();
    }

    T* get() const noexcept {
        return _uniquePtr.get().ptr();
    }

    T& operator*() noexcept {
        return *get();
    }

    T& operator*() const noexcept {
        return *get();
    }

    T* release() noexcept {
        return _uniquePtr.release().ptr();
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
        _uniquePtr = std::move(other._uniquePtr);
        return *this;
    }

private:
    std::unique_ptr<T, boost::alloc_deleter<T, Allocator<T>>> _uniquePtr;
};

template <class T, class... Args>
unique_ptr<T> make_unique(Context& Context, Args&&... args) {
    return unique_ptr<T>(Context, std::forward<Args>(args)...);
}

}  // namespace tracking
}  // namespace mongo
