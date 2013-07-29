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
        OwnedPointerVector();
        ~OwnedPointerVector();

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

        void clear();

    private:
        std::vector<T*> _vector;
    };

    template<class T>
    OwnedPointerVector<T>::OwnedPointerVector() {
    }

    template<class T>
    OwnedPointerVector<T>::~OwnedPointerVector() {
        clear();
    }

    template<class T>
    void OwnedPointerVector<T>::clear() {
        for( typename std::vector<T*>::iterator i = _vector.begin(); i != _vector.end(); ++i ) {
            delete *i;
        }
        _vector.clear();
    }

} // namespace mongo
