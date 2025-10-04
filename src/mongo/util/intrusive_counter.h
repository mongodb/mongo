/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util_core.h"
#include "mongo/util/modules_incompletely_marked_header.h"

#include <atomic>  // NOLINT
#include <cstdlib>

#include <boost/intrusive_ptr.hpp>

namespace mongo {

/// This is an alternative base class to the above ones (will replace them eventually)
class MONGO_MOD_OPEN RefCountable {
    RefCountable(const RefCountable&) = delete;
    RefCountable& operator=(const RefCountable&) = delete;

public:
    /// If false you have exclusive access to this object. This is useful for implementing COW.
    bool isShared() const {
        // This needs to be at least acquire to ensure that it is sequenced-after any
        // intrusive_ptr_release calls. Otherwise there is a subtle race where the releaser's memory
        // accesses could see writes done by a thread that thinks it has exclusive access to this
        // object. Note that acquire reads are free on x86 and cheap on most other platforms.
        return _count.load(std::memory_order_acquire) > 1;
    }

    /**
     * Sets the refcount to count, assuming it is currently one less. This should only be used
     * during logical initialization before another thread could possibly have access to this
     * object.
     */
    void threadUnsafeIncRefCountTo(uint32_t count) const {
        MONGO_COMPILER_DIAGNOSTIC_PUSH
        MONGO_COMPILER_DIAGNOSTIC_WORKAROUND_ATOMIC_READ
        dassert(_count.load(std::memory_order_relaxed) == (count - 1));
        MONGO_COMPILER_DIAGNOSTIC_POP

        MONGO_COMPILER_DIAGNOSTIC_PUSH
        MONGO_COMPILER_DIAGNOSTIC_WORKAROUND_ATOMIC_WRITE
        _count.store(count, std::memory_order_relaxed);
        MONGO_COMPILER_DIAGNOSTIC_POP
    }

    friend void intrusive_ptr_add_ref(const RefCountable* ptr) {
        // See this for a description of why relaxed is OK here. It is also used in libc++.
        // http://www.boost.org/doc/libs/1_66_0/doc/html/atomic/usage_examples.html#boost_atomic.usage_examples.example_reference_counters.discussion
        ptr->_count.fetch_add(1, std::memory_order_relaxed);
    }

    friend void intrusive_ptr_release(const RefCountable* ptr) {
        MONGO_COMPILER_DIAGNOSTIC_PUSH
        MONGO_COMPILER_DIAGNOSTIC_WORKAROUND_ATOMIC_READ
        if (ptr->_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete ptr;
        }
        MONGO_COMPILER_DIAGNOSTIC_POP
    }

protected:
    /**
     * Sets the refcount to count, assuming it is currently one more. This must be called only in
     * conjuction with intrusive_ptr::detach() to exit a scope with an intrusive_ptr without
     * destructing the pointed-to object.
     */
    void unsafeRefDecRefCountTo(uint32_t count) const {
        invariant(_count.load(std::memory_order_relaxed) == (count + 1));
        _count.store(count, std::memory_order_relaxed);
    }

    RefCountable() {}
    virtual ~RefCountable() {}

private:
    mutable std::atomic<uint32_t> _count{0};  // NOLINT
};

template <typename T,
          typename... Args,
          typename = std::enable_if_t<std::is_base_of<RefCountable, T>::value>>
boost::intrusive_ptr<T> make_intrusive(Args&&... args) {
    auto ptr = new T(std::forward<Args>(args)...);
    ptr->threadUnsafeIncRefCountTo(1);
    return boost::intrusive_ptr<T>(ptr, /*add ref*/ false);
}
}  // namespace mongo
