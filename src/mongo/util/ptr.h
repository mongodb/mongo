/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include <boost/scoped_ptr.hpp>
#include <boost/shared_ptr.hpp>
#include <memory>
#include <type_traits>

namespace mongo {


    /** A generic pointer type for function arguments.
     *  It will convert from any pointer type except auto_ptr.
     *  Semantics are the same as passing the pointer returned from get()
     *  const ptr<T>  =>  T * const
     *  ptr<const T>  =>  T const *  or  const T*
     */
    template <typename T>
    struct ptr {
        // Removes conversions from overload resolution if the underlying pointer types aren't
        // convertible. This makes this class behave more like a bare pointer.
        template <typename U>
        using IfConvertible = typename std::enable_if<std::is_convertible<U*, T*>::value>::type;

        ptr() : _p(NULL) {}

        // convert to ptr<T>
        ptr(T* p) : _p(p) {} // needed for NULL

        template<typename U, typename = IfConvertible<U>>
        ptr(U* p) : _p(p) {}

        template<typename U, typename = IfConvertible<U>>
        ptr(const ptr<U>& p) : _p(p) {}

        template<typename U, typename = IfConvertible<U>>
        ptr(const std::unique_ptr<U>& p) : _p(p.get()) {}

        template<typename U, typename = IfConvertible<U>>
        ptr(const boost::shared_ptr<U>& p) : _p(p.get()) {}

        template<typename U, typename = IfConvertible<U>>
        ptr(const boost::scoped_ptr<U>& p) : _p(p.get()) {}

        // assign to ptr<T>
        ptr& operator=(T* p) { _p = p; return *this; } // needed for NULL

        template<typename U, typename = IfConvertible<U>>
        ptr& operator=(U* p) {
            _p = p;
            return *this;
        }

        template<typename U, typename = IfConvertible<U>>
        ptr& operator=(const ptr<U>& p) {
            _p = p;
            return *this;
        }

        template<typename U, typename = IfConvertible<U>>
        ptr& operator=(const std::unique_ptr<U>& p) {
            _p = p.get();
            return *this;
        }

        template<typename U, typename = IfConvertible<U>>
        ptr& operator=(const boost::shared_ptr<U>& p) {
            _p = p.get();
            return *this;
        }

        template<typename U, typename = IfConvertible<U>>
        ptr& operator=(const boost::scoped_ptr<U>& p) {
            _p = p.get();
            return *this;
        }

        // use
        T* operator->() const { return _p; }
        T& operator*() const { return *_p; }

        // convert from ptr<T>
        operator T* () const { return _p; }

    private:
        T* _p;
    };

} // namespace mongo
