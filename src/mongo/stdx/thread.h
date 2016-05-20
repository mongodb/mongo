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

#include <exception>
#include <thread>
#include <type_traits>

namespace mongo {
namespace stdx {

/**
 * We're wrapping std::thread here, rather than aliasing it, because we'd like
 * a std::thread that's identical in all ways to the original, but terminates
 * if a new thread cannot be allocated.  We'd like this behavior because we
 * rarely if ever try/catch thread creation, and don't have a strategy for
 * retrying.  Therefore, all throwing does is remove context as to which part
 * of the system failed thread creation (as the exception itself is caught at
 * the top of the stack).
 *
 * We're putting this in stdx, rather than having it as some kind of
 * mongo::Thread, because the signature and use of the type is otherwise
 * completely identical.  Rather than migrate all callers, it was deemed
 * simpler to make the in place adjustment and retain it in stdx.
 *
 * We implement this with private inheritance to minimize the overhead of our
 * wrapping and to simplify the implementation.
 */
class thread : private ::std::thread {  // NOLINT
public:
    using ::std::thread::native_handle_type;  // NOLINT
    using ::std::thread::id;                  // NOLINT

    thread() noexcept : ::std::thread::thread() {}  // NOLINT

    thread(const thread&) = delete;

    thread(thread&& other) noexcept
        : ::std::thread::thread(static_cast<::std::thread&&>(std::move(other))) {}  // NOLINT

    /**
     * As of C++14, the Function overload for std::thread requires that this constructor only
     * participate in overload resolution if std::decay_t<Function> is not the same type as thread.
     * That prevents this overload from intercepting calls that might generate implicit conversions
     * before binding to other constructors (specifically move/copy constructors).
     */
    template <
        class Function,
        class... Args,
        typename std::enable_if<!std::is_same<thread, typename std::decay<Function>::type>::value,
                                int>::type = 0>
    explicit thread(Function&& f, Args&&... args) try:
        ::std::thread::thread(std::forward<Function>(f), std::forward<Args>(args)...) {}  // NOLINT
    catch (...) {
        std::terminate();
    }

    thread& operator=(const thread&) = delete;

    thread& operator=(thread&& other) noexcept {
        return static_cast<thread&>(
            ::std::thread::operator=(static_cast<::std::thread&&>(std::move(other))));  // NOLINT
    };

    using ::std::thread::joinable;              // NOLINT
    using ::std::thread::get_id;                // NOLINT
    using ::std::thread::native_handle;         // NOLINT
    using ::std::thread::hardware_concurrency;  // NOLINT

    using ::std::thread::join;    // NOLINT
    using ::std::thread::detach;  // NOLINT

    void swap(thread& other) noexcept {
        ::std::thread::swap(static_cast<::std::thread&>(other));  // NOLINT
    }
};

inline void swap(thread& lhs, thread& rhs) noexcept {
    lhs.swap(rhs);
}

namespace this_thread = ::std::this_thread;  // NOLINT

}  // namespace stdx
}  // namespace mongo
