// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util_core.h"
#include "mongo/util/modules.h"

#include <atomic>  // NOLINT
#include <cstdlib>

#include <boost/intrusive_ptr.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/// This is an alternative base class to the above ones (will replace them eventually)
class [[MONGO_MOD_OPEN]] RefCountable {
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
