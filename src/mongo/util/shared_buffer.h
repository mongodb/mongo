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

namespace mongo {

    class SharedBuffer {
    public:
        SharedBuffer() = default;

        void swap(SharedBuffer& other) {
            _holder.swap(other._holder);
        }

        SharedBuffer(const SharedBuffer&) = default;
        SharedBuffer& operator=(const SharedBuffer&) = default;

        SharedBuffer(SharedBuffer&& other)
            : _holder() {
            swap(other);
        }

        SharedBuffer& operator=(SharedBuffer&& other) {
            swap(other);
            other._holder.reset();
            return *this;
        }

        static SharedBuffer allocate(size_t bytes) {
            return takeOwnership(static_cast<char*>(malloc(sizeof(Holder) + bytes)));
        }

        /**
         * Given a pointer to a region of un-owned data, prefixed by sufficient space for a
         * SharedBuffer::Holder object, return an SharedBuffer that owns the
         * memory.
         *
         * This class will call free(holderPrefixedData), so it must have been allocated in a way
         * that makes that valid.
         */
        static SharedBuffer takeOwnership(char* holderPrefixedData) {
            // Initialize the refcount to 1 so we don't need to increment it in the constructor
            // (see private Holder* constructor below).
            //
            // TODO: Should dassert alignment of holderPrefixedData
            // here if possible.
            return SharedBuffer(new(holderPrefixedData) Holder(1U));
        }

        char* get() const {
            return _holder ? _holder->data() : NULL;
        }

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
                return reinterpret_cast<char *>(this + 1);
            }

            const char* data() const {
                return reinterpret_cast<const char *>(this + 1);
            }

        private:
            AtomicUInt32 _refCount;
        };

    private:
        explicit SharedBuffer(Holder* holder)
            : _holder(holder, /*add_ref=*/ false) {
            // NOTE: The 'false' above is because we have already initialized the Holder with a
            // refcount of '1' in takeOwnership above. This avoids an atomic increment.
        }

        boost::intrusive_ptr<Holder> _holder;
    };

    inline void swap(SharedBuffer& one, SharedBuffer& two) {
        one.swap(two);
    }
}
