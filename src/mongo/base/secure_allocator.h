/*    Copyright 2015 MongoDB Inc.
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

#include "mongo/config.h"

#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "mongo/stdx/type_traits.h"
#include "mongo/util/assert_util.h"

namespace mongo {

namespace secure_allocator_details {

void* allocate(std::size_t bytes, std::size_t alignOf);
void deallocate(void* ptr, std::size_t bytes);

}  // namespace secure_allocator_details

/**
 * Provides a secure allocator for trivially copyable types. By secure we mean
 * memory that will be zeroed on free and locked out of paging while in memory
 * (to prevent it from being written to disk).
 *
 * While this type can be used with any allocator aware container, it should be
 * considered whether either of the two named specializations below are
 * sufficient (a string and a vector). The allocations out of this container
 * are quite expensive, so one should endeavor to use containers which make
 * few, contiguous allocations where possible.
 *
 * Note that this allocator is written without reling on default
 * semantics injected via allocator_traits, and so defines all
 * optional allocator members, and does not rely on allocator_traits
 * to default them in. See http://stackoverflow.com/a/33267132 for a
 * rationale for GCC 4.8, our current default compiler. There is also
 * evidence that MSVC 2013's _DEBUG STL does not work correctly with
 * allocator_traits.
 *
 * See also: http://howardhinnant.github.io/allocator_boilerplate.html
 */
template <typename T>
struct SecureAllocator {
/**
 * We only support trivially copyable types to avoid situations where the
 * SecureAllocator is used in containers with complex types that do their
 * own allocation. I.e. one could otherwise have a:
 *
 * std::vector<std::string, SecureAllocator<std::string>>
 *
 * where the vectors were stored securely, but the strings spilled to the
 * heap
 *
 */
#ifdef MONGO_CONFIG_HAVE_STD_IS_TRIVIALLY_COPYABLE
    static_assert(std::is_trivially_copyable<T>::value,
                  "SecureAllocator can only be used with trivially copyable types");
#endif

    // NOTE: The standard doesn't seem to require these, but libstdc++
    // definitly wants them.
    using reference = T&;
    using const_reference = const T&;


    // NOTE: These members are defined in the same order as specified
    // in the "Allocator Requirements" section of the standard. Please
    // retain this ordering.

    using pointer = T*;
    using const_pointer = const T*;
    using void_pointer = void*;
    using const_void_pointer = const void*;
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;

    template <typename U>
    struct rebind {
        using other = SecureAllocator<U>;
    };

    pointer allocate(size_type n) {
        return static_cast<pointer>(secure_allocator_details::allocate(
            sizeof(value_type) * n, std::alignment_of<T>::value));
    }

    pointer allocate(size_type n, const_void_pointer) {
        return allocate(n);
    }

    void deallocate(pointer ptr, size_type n) {
        return secure_allocator_details::deallocate(static_cast<void*>(ptr),
                                                    sizeof(value_type) * n);
    }

    size_type max_size() {
        return std::numeric_limits<size_type>::max();
    }

    SecureAllocator() = default;

    template <typename U>
    SecureAllocator(const SecureAllocator<U>& other) {}

    template <typename U, typename... Args>
    void construct(U* p, Args&&... args) {
        ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
    }

    template <typename U>
    void destroy(U* p) {
        p->~U();
    }

    SecureAllocator select_on_container_copy_construction() {
        // SecureAllocator is stateless, so just return a default
        // constructed instance.
        return SecureAllocator();
    }

    // For background:
    //
    // http://stackoverflow.com/questions/27471053/example-usage-of-propagate-on-container-move-assignment
    //
    // https://foonathan.github.io/blog/2015/10/05/allocatorawarecontainer-propagation-pitfalls.html
    //
    // This allocator is stateless, so we can avoid a runtime check
    // (even though it would probably be optimized out based on the
    // constrexpr-esque nature of our equality comparison operator),
    // so we can set all of these to true.
    using propagate_on_container_copy_assignment = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;

    using is_always_equal = std::true_type;
};

template <typename T, typename U>
bool operator==(const SecureAllocator<T>& lhs, const SecureAllocator<U>& rhs) {
    // Note: If you change this, you must re-evaluate the select_ and
    // propagate_ methods and typedefs above.
    return true;
}

template <typename T, typename U>
bool operator!=(const SecureAllocator<T>& lhs, const SecureAllocator<U>& rhs) {
    return !(lhs == rhs);
}

/**
 * SecureHandle offers a smart pointer-ish interface to a type.
 *
 * It attempts to solve the problem of using container types with small object optimizations that
 * might accidentally leave important data on the stack if they're too small to spill to the heap.
 * It uses the secure allocator for allocations and deallocations.
 *
 * This type is meant to offer more value like semantics than a unique_ptr:
 *   - SecureHandle's are only default constructible if their T is
 *   - You can construct a SecureHandle like the underlying value:
 *       SecureHandle<Mytype> type(foo, bar baz);
 *   - SecureHandle's are copyable if their T's are
 *   - only moving can produce a non-usable T
 *
 * While still being cheap and convenient like a unique_ptr:
 *   - SecureHandle's move by pointer to their secure storage
 *   - T& operator*()
 *   - T* operator->()
 *
 * In a moved from state, SecureHandle's may be copy or move assigned and the destructor may run,
 * but all other operations will invariant.
 */
template <typename T>
class SecureHandle {
public:
    // NOTE: (jcarey)
    //
    // We have the default ctor and the perfect forwarding ctor because msvc 2013 ice's on some
    // default constructed types without it (sfinae was falling over for some reason).
    //
    // For non-default constructible t's, we'll fail to substitute the forwarded call to new
    SecureHandle() : _t(_new()) {}

    // Generic constructor that forwards to the underlying T if the first arg isn't a SecureHandle
    template <
        typename Arg,
        typename... Args,
        stdx::enable_if_t<!std::is_same<SecureHandle<T>, typename std::decay<Arg>::type>::value,
                          int> = 0>
    SecureHandle(Arg&& arg, Args&&... args)
        : _t(_new(std::forward<Arg>(arg), std::forward<Args>(args)...)) {}

    SecureHandle(const SecureHandle& other) : _t(_new(*other)) {}

    SecureHandle& operator=(const SecureHandle& other) {
        if (_t) {
            *_t = *other;
        } else {
            _t = _new(*other);
        }

        return *this;
    }

    SecureHandle(SecureHandle&& other) : _t(other._t) {
        other._t = nullptr;
    }

    SecureHandle& operator=(SecureHandle&& other) {
        if (&other == this) {
            return *this;
        }

        _delete();

        _t = other._t;
        other._t = nullptr;

        return *this;
    }

    ~SecureHandle() {
        _delete();
    }

    T& operator*() const {
        invariant(_t);

        return *_t;
    }

    T* operator->() const {
        invariant(_t);

        return _t;
    }

private:
    template <typename... Args>
    static T* _new(Args&&... args) {
        return ::new (secure_allocator_details::allocate(sizeof(T), std::alignment_of<T>::value))
            T(std::forward<Args>(args)...);
    }

    void _delete() {
        if (_t) {
            _t->~T();
            secure_allocator_details::deallocate(_t, sizeof(T));
        }
    }

    T* _t;
};

template <typename T>
using SecureVector = SecureHandle<std::vector<T, SecureAllocator<T>>>;

using SecureString =
    SecureHandle<std::basic_string<char, std::char_traits<char>, SecureAllocator<char>>>;

}  // namespace mongo
