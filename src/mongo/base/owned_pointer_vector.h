/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include <cstring>
#include <vector>

#include "mongo/base/disallow_copying.h"

namespace mongo {

    /**
     * An std::vector wrapper that deletes pointers within a vector on destruction.  The objects
     * referenced by the vector's pointers are 'owned' by an object of this class.
     * NOTE that an OwnedPointerVector<T> wraps an std::vector<T*>.
     */
    template<class T>
    class OwnedPointerVector {
        MONGO_DISALLOW_COPYING(OwnedPointerVector);

    public:
        OwnedPointerVector() {}
        ~OwnedPointerVector() { clear(); }

        /**
         * Takes ownership of all pointers contained in 'other'.
         * NOTE: argument is intentionally taken by value.
         */
        OwnedPointerVector(std::vector<T*> other) { _vector.swap(other); }

        /**
         * Takes ownership of all pointers contained in 'other'.
         * NOTE: argument is intentionally taken by value.
         */
        OwnedPointerVector& operator=(std::vector<T*> other) {
            clear();
            _vector.swap(other);
            return *this;
        }

        typedef typename std::vector<T*>::const_iterator const_iterator;
        typedef typename std::vector<T*>::const_reverse_iterator const_reverse_iterator;

        /** Access the vector. */
        const std::vector<T*>& vector() const { return _vector; }
        std::vector<T*>& mutableVector() { return _vector; }

        std::size_t size() const { return _vector.size(); }
        bool empty() const { return _vector.empty(); }

        const_iterator begin() const { return _vector.begin(); }
        const_reverse_iterator rbegin() const { return _vector.rbegin(); }
        const_iterator end() const { return _vector.end(); }
        const_reverse_iterator rend() const { return _vector.rend(); }

        T* operator[] (size_t i) const { return _vector[i]; }
        T* back() const { return _vector.back(); }
        T* front() const { return _vector.front(); }

        void push_back(T* ptr) { _vector.push_back(ptr); }

        /**
         * Deletes all pointers in the vector, then sets its size to 0.
         */
        void clear();

        /**
         * Deletes the pointer at 'it', then erases it from the vector.
         */
        void erase(const_iterator it) {
            delete *it;
            // vector::erase(const_iterator) is new in c++11, so converting to non-const iterator.
            _vector.erase(_vector.begin() + (it - begin()));
        }

        //
        // extensions
        //

        /**
         * Releases the entire vector to allow you to transfer ownership.
         *
         * Leaves the OwnedPointerVector empty.
         * Named after the similar method and pattern in std::auto_ptr.
         */
        std::vector<T*> release() {
            std::vector<T*> out;
            out.swap(_vector);
            return out;
        }

        /**
         * Releases ownership of a single element.
         *
         * Sets that element to NULL and does not change size().
         */
        T* releaseAt(size_t i) {
            T* out = _vector[i];
            _vector[i] = NULL;
            return out;
        }

        T* popAndReleaseBack() {
            T* out = _vector.back();
            _vector.pop_back();
            return out;
        }

        void popAndDeleteBack() {
            delete popAndReleaseBack();
        }

    private:
        std::vector<T*> _vector;
    };

    template<class T>
    inline void OwnedPointerVector<T>::clear() {
        for( typename std::vector<T*>::iterator i = _vector.begin(); i != _vector.end(); ++i ) {
            delete *i;
        }
        _vector.clear();
    }

} // namespace mongo
