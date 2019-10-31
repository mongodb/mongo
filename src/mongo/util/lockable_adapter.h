/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

namespace mongo {

// BasicLockableAdapter allows non-template functions to take any lockable type. This can be useful
// when you have a custom lockable type and don't want to make the lockable parameter concrete for a
// function but can't make the function use a template parameter for the lock type.
//
// This type should NOT be used to store a lockable type!
//
// Example:
//      void wait(BasicLockableAdapter lock) {
//          stdx::lock_guard lg(lock);
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
                                       +[](void* t) { static_cast<T*>(t)->unlock(); }};

    void* _underlyingLock;
    const VTable* _vtable;
};

}  // namespace mongo
