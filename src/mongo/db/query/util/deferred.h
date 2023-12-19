/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <functional>

namespace mongo {

/**
 * A template class that provides a way to defer the initialization of an object until its value is
 * actually required. This is also commonly referred to as lazy initialization.
 *
 * Dangers:
 * - This implementation is currently not thread safe, and it shouldn't be used in multi-threaded
 *   fashion.
 * - Be careful about using this for lazy initialization of data members and capturing the 'this'
 *   variable. Code like this will result in buggy/unsafe move constructors, which would have a
 *   dangling reference to the moved-from type:
 *
 *   class MyType {
 *       int x;
 *       // !!! Dangling 'this' when moved !!!
 *       Deferred<int> xSquared{[this]() { return this->x * this-> x; };
 *   };
 *   Instead, it is better to do something like this:
 *   class MyType {
 *       int xSquared() const {
 *          return *_xSquared.get(_x);
 *       }
 *
 *       int _x;
 *       Deferred<int, int> _xSquared{[](int x) { return x * x; };
 *   };
 * - As a similar danger, the value is only computed once. if you initialize it with arguments like
 *   the above 'xSquared()' implementation, then be cogniscent that the value will never change. If
 *   '_x' changes, '_xSquared' will not.
 *
 * A Deferred class can be constructed with either an initial value (eager initialization) or a
 * function that will generate the value when needed.
 */
template <typename T, typename... Args>
class Deferred {
public:
    /**
     * Instantiates a Deffered<T> with the given data - no callbacks or lazy initialization.
     */
    Deferred(T data) : _data(data) {}

    /**
     * Stores a function to compute a T later. Please note the warnings described in this class
     * comment.
     */
    Deferred(std::function<T(Args&&...)> initializer) : _initializer(std::move(initializer)) {}

    /**
     * Returns a pointer to the managed object. Initializes the object if it hasn't done so already.
     */
    T& get(Args&&... args) const {
        if (_initializer) {
            _data = _initializer(std::forward<Args>(args)...);
            _initializer = nullptr;
        }
        return _data;
    }

    /**
     * Dereferences the pointer to the managed object. Note this is only a valid shortcut if there
     * are no arguments to '_initializer'.
     */
    T* operator->() const {
        return &get();
    }

    /**
     * Returns a referenced to the managed object. Initializes the object if it hasn't done so
     * already. Note this is only a valid shortcut if there are no arguments to '_initializer'.
     */
    const T& operator*() const {
        return get();
    }

    bool isInitialized() const {
        return _initializer ? false : true;
    }

private:
    mutable T _data;
    mutable std::function<T(Args&&...)> _initializer;
};

}  // namespace mongo
