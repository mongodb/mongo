
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

#include <boost/intrusive_ptr.hpp>
#include <stdlib.h>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/allocator.h"

namespace mongo {

/// This is an alternative base class to the above ones (will replace them eventually)
class RefCountable {
    MONGO_DISALLOW_COPYING(RefCountable);

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
        dassert(_count.load(std::memory_order_relaxed) == (count - 1));
        _count.store(count, std::memory_order_relaxed);
    }

    friend void intrusive_ptr_add_ref(const RefCountable* ptr) {
        // See this for a description of why relaxed is OK here. It is also used in libc++.
        // http://www.boost.org/doc/libs/1_66_0/doc/html/atomic/usage_examples.html#boost_atomic.usage_examples.example_reference_counters.discussion
        ptr->_count.fetch_add(1, std::memory_order_relaxed);
    };

    friend void intrusive_ptr_release(const RefCountable* ptr) {
        if (ptr->_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete ptr;
        }
    };

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

/// This is an immutable reference-counted string
class RCString : public RefCountable {
public:
    const char* c_str() const {
        return reinterpret_cast<const char*>(this) + sizeof(RCString);
    }
    int size() const {
        return _size;
    }
    StringData stringData() const {
        return StringData(c_str(), _size);
    }

    static boost::intrusive_ptr<const RCString> create(StringData s);

// MSVC: C4291: 'declaration' : no matching operator delete found; memory will not be freed if
// initialization throws an exception
// We simply rely on the default global placement delete since a local placement delete would be
// ambiguous for some compilers
#pragma warning(push)
#pragma warning(disable : 4291)
    void operator delete(void* ptr) {
        free(ptr);
    }
#pragma warning(pop)

private:
    // these can only be created by calling create()
    RCString(){};
    void* operator new(size_t objSize, size_t realSize) {
        return mongoMalloc(realSize);
    }

    int _size;  // does NOT include trailing NUL byte.
    // char[_size+1] array allocated past end of class
};
}  // namespace mongo
