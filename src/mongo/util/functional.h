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

#include <functional>
#include <type_traits>

#include "mongo/stdx/type_traits.h"
#include "mongo/util/assert_util.h"

namespace mongo {
template <typename Function>
class unique_function;

/**
 * A `unique_function` is a move-only, type-erased functor object similar to `std::function`.
 * It is useful in situations where a functor cannot be wrapped in `std::function` objects because
 * it is incapable of being copied.  Often this happens with C++14 or later lambdas which capture a
 * `std::unique_ptr` by move.  The interface of `unique_function` is nearly identical to
 * `std::function`, except that it is not copyable.
 */
template <typename RetType, typename... Args>
class unique_function<RetType(Args...)> {
private:
    // `TagTypeBase` is used as a base for the `TagType` type, to prevent it from being an
    // aggregate.
    struct TagTypeBase {
    protected:
        TagTypeBase() = default;
    };
    // `TagType` is used as a placeholder type in parameter lists for `enable_if` clauses.  They
    // have to be real parameters, not template parameters, due to MSVC limitations.
    class TagType : TagTypeBase {
        TagType() = default;
        friend unique_function;
    };

public:
    using result_type = RetType;

    ~unique_function() noexcept = default;
    unique_function() = default;

    unique_function(const unique_function&) = delete;
    unique_function& operator=(const unique_function&) = delete;

    unique_function(unique_function&&) noexcept = default;
    unique_function& operator=(unique_function&&) noexcept = default;

    void swap(unique_function& that) noexcept {
        using std::swap;
        swap(this->impl, that.impl);
    }

    friend void swap(unique_function& a, unique_function& b) noexcept {
        a.swap(b);
    }

    // TODO: Look into creating a mechanism based upon a unique_ptr to `void *`-like state, and a
    // `void *` accepting function object.  This will permit reusing the core impl object when
    // converting between related function types, such as
    // `int (std::string)` -> `void (const char *)`
    template <typename Functor>
    /* implicit */
    unique_function(
        Functor&& functor,
        // The remaining arguments here are only for SFINAE purposes to enable this ctor when our
        // requirements are met.  They must be concrete parameters not template parameters to work
        // around bugs in some compilers that we presently use.  We may be able to revisit this
        // design after toolchain upgrades for C++17.
        std::enable_if_t<stdx::is_invocable_r<RetType, Functor, Args...>::value, TagType> =
            makeTag(),
        std::enable_if_t<std::is_move_constructible<Functor>::value, TagType> = makeTag(),
        std::enable_if_t<!std::is_same<Functor, unique_function>::value, TagType> = makeTag())
        : impl(makeImpl(std::forward<Functor>(functor))) {}

    unique_function(std::nullptr_t) noexcept {}

    RetType operator()(Args... args) const {
        invariant(static_cast<bool>(*this));
        return impl->call(std::forward<Args>(args)...);
    }

    explicit operator bool() const noexcept {
        return static_cast<bool>(this->impl);
    }

    // Needed to make `std::is_convertible<mongo::unique_function<...>, std::function<...>>` be
    // `std::false_type`.  `mongo::unique_function` objects are not convertible to any kind of
    // `std::function` object, since the latter requires a copy constructor, which the former does
    // not provide.  If you see a compiler error which references this line, you have tried to
    // assign a `unique_function` object to a `std::function` object which is impossible -- please
    // check your variables and function signatures.
    //
    // NOTE: This is not quite able to disable all `std::function` conversions on MSVC, at this
    // time.
    template <typename Signature>
    operator std::function<Signature>() const = delete;

private:
    // The `TagType` type cannot be constructed as a default function-parameter in Clang.  So we use
    // a static member function that initializes that default parameter.
    static TagType makeTag() {
        return {};
    }

    struct Impl {
        virtual ~Impl() noexcept = default;
        virtual RetType call(Args&&... args) = 0;
    };

    // These overload helpers are needed to squelch problems in the `T ()` -> `void ()` case.
    template <typename Functor>
    static void callRegularVoid(const std::true_type isVoid, Functor& f, Args&&... args) {
        // The result of this call is not cast to void, to help preserve detection of
        // `[[nodiscard]]` violations.
        f(std::forward<Args>(args)...);
    }

    template <typename Functor>
    static RetType callRegularVoid(const std::false_type isNotVoid, Functor& f, Args&&... args) {
        return f(std::forward<Args>(args)...);
    }

    template <typename Functor>
    static auto makeImpl(Functor&& functor) {
        struct SpecificImpl : Impl {
            explicit SpecificImpl(Functor&& func) : f(std::forward<Functor>(func)) {}

            RetType call(Args&&... args) override {
                return callRegularVoid(std::is_void<RetType>(), f, std::forward<Args>(args)...);
            }

            std::decay_t<Functor> f;
        };

        return std::make_unique<SpecificImpl>(std::forward<Functor>(functor));
    }

    std::unique_ptr<Impl> impl;
};

template <typename Signature>
bool operator==(const unique_function<Signature>& lhs, std::nullptr_t) noexcept {
    return !lhs;
}

template <typename Signature>
bool operator!=(const unique_function<Signature>& lhs, std::nullptr_t) noexcept {
    return static_cast<bool>(lhs);
}

template <typename Signature>
bool operator==(std::nullptr_t, const unique_function<Signature>& rhs) noexcept {
    return !rhs;
}

template <typename Signature>
bool operator!=(std::nullptr_t, const unique_function<Signature>& rhs) noexcept {
    return static_cast<bool>(rhs);
}
}  // namespace mongo
