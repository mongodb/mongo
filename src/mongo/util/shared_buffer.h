/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <boost/intrusive_ptr.hpp>

#include "mongo/platform/atomic_word.h"
#include "mongo/util/allocator.h"
#include "mongo/util/assert_util.h"

namespace mongo {

/**
 * A mutable, ref-counted buffer.
 */
class SharedBuffer {
public:
    SharedBuffer() = default;

    void swap(SharedBuffer& other) {
        _holder.swap(other._holder);
    }

    static SharedBuffer allocate(size_t bytes) {
        return takeOwnership(mongoMalloc(sizeof(Holder) + bytes));
    }

    /**
     * Resizes the buffer, copying the current contents.
     *
     * Like ::realloc() this can be called on a null SharedBuffer.
     *
     * This method is illegal to call if any other SharedBuffer instances share this buffer since
     * they wouldn't be updated and would still try to delete the original buffer.
     */
    void realloc(size_t size) {
        invariant(!_holder || !_holder->isShared());

        const size_t realSize = size + sizeof(Holder);
        void* newPtr = mongoRealloc(_holder.get(), realSize);

        // Get newPtr into _holder with a ref-count of 1 without touching the current pointee of
        // _holder which is now invalid.
        auto tmp = SharedBuffer::takeOwnership(newPtr);
        _holder.detach();
        _holder = std::move(tmp._holder);
    }

    char* get() const {
        return _holder ? _holder->data() : NULL;
    }

    explicit operator bool() const {
        return bool(_holder);
    }

private:
    class Holder {
    public:
        explicit Holder(AtomicUInt32::WordType initial = AtomicUInt32::WordType())
            : _refCount(initial) {}

        // these are called automatically by boost::intrusive_ptr
        friend void intrusive_ptr_add_ref(Holder* h) {
            h->_refCount.fetchAndAdd(1);
        }

        friend void intrusive_ptr_release(Holder* h) {
            if (h->_refCount.subtractAndFetch(1) == 0) {
                // We placement new'ed a Holder in takeOwnership above,
                // so we must destroy the object here.
                h->~Holder();
                free(h);
            }
        }

        char* data() {
            return reinterpret_cast<char*>(this + 1);
        }

        const char* data() const {
            return reinterpret_cast<const char*>(this + 1);
        }

        bool isShared() const {
            return _refCount.load() > 1;
        }

        AtomicUInt32 _refCount;
    };

    explicit SharedBuffer(Holder* holder) : _holder(holder, /*add_ref=*/false) {
        // NOTE: The 'false' above is because we have already initialized the Holder with a
        // refcount of '1' in takeOwnership below. This avoids an atomic increment.
    }

    /**
     * Given a pointer to a region of un-owned data, prefixed by sufficient space for a
     * SharedBuffer::Holder object, return an SharedBuffer that owns the memory.
     *
     * This class will call free(holderPrefixedData), so it must have been allocated in a way
     * that makes that valid.
     */
    static SharedBuffer takeOwnership(void* holderPrefixedData) {
        // Initialize the refcount to 1 so we don't need to increment it in the constructor
        // (see private Holder* constructor above).
        //
        // TODO: Should dassert alignment of holderPrefixedData here if possible.
        return SharedBuffer(new (holderPrefixedData) Holder(1U));
    }

    boost::intrusive_ptr<Holder> _holder;
};

inline void swap(SharedBuffer& one, SharedBuffer& two) {
    one.swap(two);
}

/**
 * A constant view into a ref-counted buffer.
 *
 * Use SharedBuffer to allocate since allocating a const buffer is useless.
 */
class ConstSharedBuffer {
public:
    ConstSharedBuffer() = default;
    /*implicit*/ ConstSharedBuffer(SharedBuffer source) : _buffer(std::move(source)) {}

    void swap(ConstSharedBuffer& other) {
        _buffer.swap(other._buffer);
    }

    const char* get() const {
        return _buffer.get();
    }

    explicit operator bool() const {
        return bool(_buffer);
    }

private:
    SharedBuffer _buffer;
};

inline void swap(ConstSharedBuffer& one, ConstSharedBuffer& two) {
    one.swap(two);
}
}
