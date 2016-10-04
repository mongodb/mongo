/*    Copyright 2012 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
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
template <class T>
class OwnedPointerVector {
    MONGO_DISALLOW_COPYING(OwnedPointerVector);

public:
    OwnedPointerVector() {}
    ~OwnedPointerVector() {
        clear();
    }

    /**
     * Takes ownership of all pointers contained in 'other'.
     * NOTE: argument is intentionally taken by value.
     */
    OwnedPointerVector(std::vector<T*> other) {
        _vector.swap(other);
    }

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
    const std::vector<T*>& vector() const {
        return _vector;
    }
    std::vector<T*>& mutableVector() {
        return _vector;
    }

    std::size_t size() const {
        return _vector.size();
    }
    bool empty() const {
        return _vector.empty();
    }

    const_iterator begin() const {
        return _vector.begin();
    }
    const_reverse_iterator rbegin() const {
        return _vector.rbegin();
    }
    const_iterator end() const {
        return _vector.end();
    }
    const_reverse_iterator rend() const {
        return _vector.rend();
    }

    T* operator[](size_t i) const {
        return _vector[i];
    }
    T* back() const {
        return _vector.back();
    }
    T* front() const {
        return _vector.front();
    }

    void push_back(T* ptr) {
        _vector.push_back(ptr);
    }

    /**
     * Deletes all pointers in the vector, then sets its size to 0.
     */
    void clear();

    /**
     * Deletes the pointer at 'it', then erases it from the vector.
     */
    void erase(const_iterator it) {
        delete *it;
        _vector.erase(toNonConstIter(it));
    }

    void erase(const_iterator begin, const_iterator end) {
        for (const_iterator it = begin; it != end; ++it) {
            delete *it;
        }
        _vector.erase(toNonConstIter(begin), toNonConstIter(end));
    }

    //
    // extensions
    //

    /**
     * Releases the entire vector to allow you to transfer ownership.
     *
     * Leaves the OwnedPointerVector empty.
     * Named after the similar method and pattern in std::unique_ptr.
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
    typename std::vector<T*>::iterator toNonConstIter(const_iterator it) {
        // This is needed for a few cases where c++03 vectors require non-const iterators that
        // were relaxed in c++11 to allow const_iterators. It can go away when we require c++11.
        return _vector.begin() + (it - begin());
    }

    std::vector<T*> _vector;
};

template <class T>
inline void OwnedPointerVector<T>::clear() {
    for (typename std::vector<T*>::iterator i = _vector.begin(); i != _vector.end(); ++i) {
        delete *i;
    }
    _vector.clear();
}

}  // namespace mongo
