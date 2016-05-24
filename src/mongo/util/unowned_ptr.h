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

#include <memory>
#include <type_traits>

namespace mongo {

/**
 * A "smart" pointer that explicitly indicates a lack of ownership.
 * It will implicitly convert from any compatible pointer type except auto_ptr.
 *
 * Note that like other pointer types const applies to the pointer not the pointee:
 * - const unowned_ptr<T>  =>  T* const
 * - unowned_ptr<const T>  =>  const T*
 */
template <typename T>
struct unowned_ptr {
    unowned_ptr() = default;

    //
    // Implicit conversions from compatible pointer types
    //

    // Removes conversions from overload resolution if the underlying pointer types aren't
    // convertible. This makes this class behave more like a bare pointer.
    template <typename U>
    using IfConvertibleFrom = typename std::enable_if<std::is_convertible<U*, T*>::value>::type;

    // Needed for NULL since it won't match U* constructor.
    unowned_ptr(T* p) : _p(p) {}

    template <typename U, typename = IfConvertibleFrom<U>>
    unowned_ptr(U* p) : _p(p) {}

    template <typename U, typename = IfConvertibleFrom<U>>
    unowned_ptr(const unowned_ptr<U>& p) : _p(p) {}

    template <typename U, typename Deleter, typename = IfConvertibleFrom<U>>
    unowned_ptr(const std::unique_ptr<U, Deleter>& p) : _p(p.get()) {}

    template <typename U, typename = IfConvertibleFrom<U>>
    unowned_ptr(const std::shared_ptr<U>& p) : _p(p.get()) {}

    //
    // Modifiers
    //

    void reset(unowned_ptr p = nullptr) {
        _p = p.get();
    }
    void swap(unowned_ptr& other) {
        std::swap(_p, other._p);
    }

    //
    // Accessors
    //

    T* get() const {
        return _p;
    }
    operator T*() const {
        return _p;
    }

    //
    // Pointer syntax
    //

    T* operator->() const {
        return _p;
    }
    T& operator*() const {
        return *_p;
    }

private:
    T* _p = nullptr;
};

}  // namespace mongo
