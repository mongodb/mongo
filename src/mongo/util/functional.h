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
#include "mongo/util/concepts.h"

namespace mongo {
template <typename Function>
class function_ref;

/**
 * A function_ref is a type-erased callable similar to std::function, however it does not own the
 * underlying object, similar to StringData vs std::string. It should generally only be used as a
 * parameter to functions that invoke their callback while running. It should generally not be put
 * in a variable or stashed for calling later.
 *
 * In the specific case of a function_ref constructed from a function or function pointer it will
 * store the function pointer directly rather than a pointer to the function pointer, so you do not
 * need to keep function pointers alive.
 *
 * function_refs are intended to be passed by value.
 *
 * Like a reference, this type has no "null" state. It is not default constructable, and moves are
 * (trivial) copies.
 *
 * This API is based on the proposed std::function_ref from https://wg21.link/P0792. It was at R8 at
 * the time this class was initially written.
 */
template <typename RetType, typename... Args>
class function_ref<RetType(Args...)> {
public:
    TEMPLATE(typename F)
    REQUIRES(std::is_invocable_r_v<RetType, F&, Args...> &&
             !std::is_same_v<stdx::remove_cvref_t<F>, function_ref>)
    /*implicit*/ function_ref(F&& f) noexcept {
        // removing then re-adding pointer ensures that (language-level) function references and
        // function pointer are treated the same.
        using Pointer = std::add_pointer_t<std::remove_pointer_t<std::remove_reference_t<F>>>;

        // Using reinterpret_cast rather than static cast to map to and from void* in order to
        // support function pointers. For object pointers, reinterpret_cast is defined by doing as
        // static_cast through a void* anyway, so this isn't actually doing anything different in
        // that case.

        if constexpr (std::is_function_v<std::remove_pointer_t<Pointer>>) {
            Pointer pointer = f;  // allow function references to decay to function pointer.
            _target = reinterpret_cast<void*>(pointer);  // store the pointer directly in _target.
        } else {
            // Make sure we didn't lose any important qualifications.
            static_assert(std::is_same_v<Pointer, decltype(&f)>);
            _target = reinterpret_cast<void*>(&f);
        }

        _adapter = +[](const void* data, Args... args) -> RetType {
            // The reinterpret_cast will add-back the const qualification removed by the const_cast
            // if F is const-qualified. This means that func will have the same const-qualifications
            // as the object passed into the constructor. The const_cast is needed in order to
            // support non-const callable objects correctly. An alternative would be to make _target
            // a non-const void*, but then we would need to remove const in the constructor and this
            // seemed cleaner.
            auto func = reinterpret_cast<Pointer>(const_cast<void*>(data));
            if constexpr (std::is_void_v<RetType>) {
                // Implicitly ignore the return. This avoids issues if func() returns a value,
                // while ensuring we still get a warning if the value is [[nodiscard]].
                (*func)(std::forward<Args>(args)...);
            } else {
                return (*func)(std::forward<Args>(args)...);
            }
        };
    }

    function_ref(const function_ref&) noexcept = default;
    function_ref& operator=(const function_ref&) noexcept = default;

    /**
     * function_ref<Sig> may only be assigned from a function_ref<Sig> (with an identical Sig), or a
     * function pointer/reference.
     *
     * Other cases are likely to dangle because they may capture a reference to a temporary that is
     * about to be destoyed, and unlike in the case we are implicitly constructing an argument, we
     * can't usefully use the function_ref before that happens (ignoring comma shenanigans). If
     * somebody really needs it, we could try to allow T& but not T&&, since T& is less likely to
     * dangle, but I don't think there is an actual use case for this, so not doing it at this time.
     */
    TEMPLATE(typename T)
    REQUIRES(!std::is_function_v<std::remove_pointer_t<T>>)
    function_ref& operator=(T) = delete;

    RetType operator()(Args... args) const {
        return _adapter(_target, std::forward<Args>(args)...);
    }

private:
    // Optimization note: An argument could be made for putting the arguments first and the data
    // last. That would mean that each argument is in the same slot it will need to be so that we
    // don't need to waste instructions sliding them around in registers. However, a very common
    // case is lambdas, and in particular lambdas like this:
    //    [&](SomeArgs args) { return this->method(args); }
    // Since that lambda is likely to be directly inlined into the type-erasure lambda, and
    // function_ref::operator() is likely to be inlined into its caller, the current argument order
    // will result in the arguments being in the correct slots, with the only fixup being to replace
    // the data pointer with the stored this pointer, since in most ABIs the implicit argument
    // parameter is treated as if it were the first argument.
    // There is also a trade-off of Args vs Args&&. The former is more efficient for trivially
    // copiable types like int and StringData, but the latter is better for expensive-to-move types
    // like std::string. I opted for the former so that this is cheap when doing cheap things and
    // because you can always pass expensive-to-move types by reference if you want to, but if we
    // added a reference here, you couldn't remove it.
    using Erased = RetType(const void*, Args...);

    const void* _target;
    Erased* _adapter;
};

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
        std::enable_if_t<std::is_invocable_r<RetType, Functor, Args...>::value, TagType> =
            makeTag(),
        std::enable_if_t<std::is_move_constructible<Functor>::value, TagType> = makeTag(),
        std::enable_if_t<!std::is_same<std::decay_t<Functor>, unique_function>::value, TagType> =
            makeTag())
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
    operator std::function<Signature>() = delete;
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

    template <typename Functor>
    static auto makeImpl(Functor&& functor) {
        struct SpecificImpl : Impl {
            explicit SpecificImpl(Functor&& func) : f(std::forward<Functor>(func)) {}

            RetType call(Args&&... args) override {
                if constexpr (std::is_void_v<RetType>) {
                    // Implicitly ignore the return. This avoids issues if func() returns a value,
                    // while ensuring we still get a warning if the value is [[nodiscard]].
                    f(std::forward<Args>(args)...);
                } else {
                    return f(std::forward<Args>(args)...);
                }
            }

            std::decay_t<Functor> f;
        };

        return std::make_unique<SpecificImpl>(std::forward<Functor>(functor));
    }

    std::unique_ptr<Impl> impl;
};

/**
 * Helper to pattern-match the signatures for all combinations of const and l-value-qualifed member
 * function pointers. We don't currently support r-value-qualified call operators.
 */
template <typename>
struct UFDeductionHelper {};
template <typename Class, typename Ret, typename... Args>
struct UFDeductionHelper<Ret (Class::*)(Args...)> : stdx::type_identity<Ret(Args...)> {};
template <typename Class, typename Ret, typename... Args>
struct UFDeductionHelper<Ret (Class::*)(Args...)&> : stdx::type_identity<Ret(Args...)> {};
template <typename Class, typename Ret, typename... Args>
struct UFDeductionHelper<Ret (Class::*)(Args...) const> : stdx::type_identity<Ret(Args...)> {};
template <typename Class, typename Ret, typename... Args>
struct UFDeductionHelper<Ret (Class::*)(Args...) const&> : stdx::type_identity<Ret(Args...)> {};

/**
 * Deduction guides for unique_function<Sig> that pluck the signature off of function pointers and
 * non-overloaded, non-generic function objects such as lambdas that don't use `auto` arguments.
 */
template <typename Ret, typename... Args>
unique_function(Ret (*)(Args...))->unique_function<Ret(Args...)>;
template <typename T, typename Sig = typename UFDeductionHelper<decltype(&T::operator())>::type>
unique_function(T)->unique_function<Sig>;

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
