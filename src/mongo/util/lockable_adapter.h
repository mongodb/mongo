// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <type_traits>

namespace [[MONGO_MOD_PUBLIC]] mongo {

// BasicLockableAdapter allows non-template functions to take any lockable type. This can be useful
// when you have a custom lockable type and don't want to make the lockable parameter concrete for a
// function but can't make the function use a template parameter for the lock type.
//
// This type should NOT be used to store a lockable type!
//
// Example:
//      void wait(BasicLockableAdapter lock) {
//          std::lock_guard lg(lock);
//      }
//
//      mongo::ResourceMutex mut;
//      wait(mut);
class BasicLockableAdapter {
public:
    template <typename T>
    BasicLockableAdapter(T& lock) : _underlyingLock(&lock), _vtable(&forT<std::decay_t<T>>) {}

    void lock() {
        _vtable->lock(_underlyingLock);
    }

    void unlock() {
        _vtable->unlock(_underlyingLock);
    }

private:
    struct VTable {
        void (*lock)(void*);
        void (*unlock)(void*);
    };

    template <typename T>
    static inline VTable forT = VTable{+[](void* t) { static_cast<T*>(t)->lock(); },
                                       +[](void* t) {
                                           static_cast<T*>(t)->unlock();
                                       }};

    void* _underlyingLock;
    const VTable* _vtable;
};

}  // namespace mongo
