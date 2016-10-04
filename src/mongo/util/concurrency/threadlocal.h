#pragma once

/**
*    Copyright (C) 2011 10gen Inc.
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

#include <boost/thread/tss.hpp>

#include "mongo/config.h"

#include "mongo/base/disallow_copying.h"

#if defined(MONGO_CONFIG_HAVE_THREAD_LOCAL)
#define MONGO_TRIVIALLY_CONSTRUCTIBLE_THREAD_LOCAL thread_local
#elif defined(MONGO_CONFIG_HAVE___THREAD)
#define MONGO_TRIVIALLY_CONSTRUCTIBLE_THREAD_LOCAL __thread
#elif defined(MONGO_CONFIG_HAVE___DECLSPEC_THREAD)
#define MONGO_TRIVIALLY_CONSTRUCTIBLE_THREAD_LOCAL __declspec(thread)
#else
#error "Compiler must support trivially constructible thread local variables"
#endif

namespace mongo {

using boost::thread_specific_ptr;

/**
 * DEPRECATED, DO NOT USE.
 *
 * thread local "value" rather than a pointer
 * good for things which have copy constructors (and the copy constructor is fast enough)
 * e.g.
 *   ThreadLocalValue<int> myint;
 */
template <class T>
class ThreadLocalValue {
public:
    ThreadLocalValue(T def = 0) : _default(def) {}

    T get() const {
        T* val = _val.get();
        if (val)
            return *val;
        return _default;
    }

    void set(const T& i) {
        T* v = _val.get();
        if (v) {
            *v = i;
            return;
        }
        v = new T(i);
        _val.reset(v);
    }

    T& getRef() {
        T* v = _val.get();
        if (v) {
            return *v;
        }
        v = new T(_default);
        _val.reset(v);
        return *v;
    }

private:
    boost::thread_specific_ptr<T> _val;
    const T _default;
};

/**
 * Emulation of a thread local storage specifier used because not all supported
 * compiler toolchains support the C++11 thread_local storage class keyword.
 *
 * Since all supported toolchains do support a thread local storage class for
 * types that are trivially constructible and destructible, we perform the
 * emulation using that storage type and the machinery of boost::thread_specific_ptr to
 * handle deleting the objects on thread termination.
 */
template <class T>
class TSP {
    MONGO_DISALLOW_COPYING(TSP);

public:
    TSP() = default;
    T* get() const;
    void reset(T* v);
    T* getMake() {
        T* t = get();
        if (t == nullptr)
            reset(t = new T());
        return t;
    }

private:
    boost::thread_specific_ptr<T> tsp;
};

#define TSP_DECLARE(T, p) extern TSP<T> p;

#define TSP_DEFINE(T, p)                                \
    MONGO_TRIVIALLY_CONSTRUCTIBLE_THREAD_LOCAL T* _##p; \
    TSP<T> p;                                           \
    template <>                                         \
    T* TSP<T>::get() const {                            \
        return _##p;                                    \
    }                                                   \
    template <>                                         \
    void TSP<T>::reset(T* v) {                          \
        tsp.reset(v);                                   \
        _##p = v;                                       \
    }
}  // namespace mongo
