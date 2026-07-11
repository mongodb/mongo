// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"
#include "mongo/util/static_immortal.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace [[MONGO_MOD_PUBLIC]] mongo {

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
class [[MONGO_MOD_PRIVATE]] ThreadNameRef {
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

    operator std::string_view() const {
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
[[MONGO_MOD_PRIVATE]] ThreadNameRef getThreadNameRef();

/**
 * Swaps in a new active name, returns the old one if it was active.
 *
 * The active thread name is used for:
 * - Setting the thread name in the OS. As an optimization, clearing
 *   the thread name in the OS is performed lazily.
 * - Populating the "ctx" field for log lines.
 * - Providing a thread name to GDB.
 */
[[MONGO_MOD_PRIVATE]] ThreadNameRef setThreadNameRef(ThreadNameRef name);

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
[[MONGO_MOD_PRIVATE]] void releaseThreadNameRef();

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
inline std::string_view getThreadName() {
    return *getThreadNameRef();
}

}  // namespace mongo
