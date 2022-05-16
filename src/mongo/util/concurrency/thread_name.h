/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <memory>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/util/static_immortal.h"

namespace mongo {

/**
 * A nullable handle pinning a ref-counted immutable string.
 * Copies of a ThreadNameString refer to the same string object.
 * Equality comparisons consider only that string's identity, not its value.
 *
 * This class is just a kind of refcounted string handle and does not itself
 * interact with the OS or with thread storage.
 *
 * Presents a pointer-like API with `get()`, and dereference operators, and
 * explicit bool conversion. Dereferencing yields a reference to a
 * string value if nonempty. Dereferencing an empty reference is allowed and
 * yields the singleton string value "-".
 *
 * Copyable and movable, with the usual refcounting semantics. Copies refer
 * to the same string and will compare equal to each other.
 *
 */
class ThreadNameRef {
public:
    /** An empty ref (empty refs still stringify as "-"). */
    ThreadNameRef() = default;

    /** A ref to the string value `name`. */
    explicit ThreadNameRef(std::string name)
        : _ptr{std::make_shared<std::string>(std::move(name))} {}

    /**
     * Dereferences this. If nonempty, returns its string value.
     * Otherwise, returns a singleton "-" string.
     */
    const std::string* get() const {
        if (_ptr)
            return &*_ptr;
        static const StaticImmortal whenEmpty = std::string("-");
        return &*whenEmpty;
    }

    const std::string* operator->() const {
        return get();
    }

    const std::string& operator*() const {
        return *get();
    }

    /** Returns true if nonempty. */
    explicit operator bool() const {
        return !!_ptr;
    }

    operator StringData() const {
        return **this;
    }

    /**
     * Two ThreadNameRef are equal if and only if they are copies of the same
     * original ThreadNameRef object. Equality of string value is insufficient.
     */
    friend bool operator==(const ThreadNameRef& a, const ThreadNameRef& b) noexcept {
        return a._ptr == b._ptr;
    }

    friend bool operator!=(const ThreadNameRef& a, const ThreadNameRef& b) noexcept {
        return !(a == b);
    }

private:
    std::shared_ptr<const std::string> _ptr;
};

/**
 * Returns the name reference attached to current thread.
 * The empty ThreadNameRef still has a valid string value of "-".
 *
 * This string is not limited in length, so it will be a better name
 * than the name the OS uses to refer to the same thread.
 */
ThreadNameRef getThreadNameRef();

/**
 * Swaps in a new active name, returns the old one if it was active.
 *
 * The active thread name is used for:
 * - Setting the thread name in the OS. As an optimization, clearing
 *   the thread name in the OS is performed lazily.
 * - Populating the "ctx" field for log lines.
 * - Providing a thread name to GDB.
 */
ThreadNameRef setThreadNameRef(ThreadNameRef name);

/**
 * Marks the ThreadNameRef attached to the current thread as inactive.
 *  - The inactive thread name remains attached to the thread.
 *  - The thread name according to the OS is not changed.
 *  - A subsequent `setThreadNameRef` call will not return it.
 *  - An immediately subsequent `setThreadNameRef` call with the same name will
 *    cheaply reactivate it, saving two OS thread rename operations.
 * This is an optimization on the assumption that a thread name will be
 * temporarily set to the same `ThreadNameRef` repeatedly, so setting it and
 * resetting it with the OS on each change would be wasteful.
 */
void releaseThreadNameRef();

/**
 * Sets the name of the current thread.
 */
inline void setThreadName(std::string name) {
    setThreadNameRef(ThreadNameRef{std::move(name)});
}

/**
 * Returns current thread's name, as previously set, or "main", or
 * "thread#" if no name was previously set.
 *
 * Used by the MongoDB GDB pretty printer extentions in `gdb/mongo.py`.
 */
inline StringData getThreadName() {
    return *getThreadNameRef();
}

}  // namespace mongo
