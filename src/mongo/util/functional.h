// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/stdx/type_traits.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <functional>
#include <iosfwd>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>


namespace [[MONGO_MOD_PUBLIC]] mongo {
template <typename Function>
class function_ref;

/**
 * A function_ref is a type-erased callable similar to std::function, however it does not own the
 * underlying object, similar to std::string_view vs std::string. It should generally only be used
 * as a parameter to functions that invoke their callback while running. It should generally not be
 * put in a variable or stashed for calling later.
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
    /** Implicitly convertible from any `f` callable with signature `RetType f(Args...)`. */
    template <typename F>
    requires(std::is_invocable_r_v<RetType, F&, Args...> &&
             !std::is_same_v<std::remove_cvref_t<F>, function_ref>)
    function_ref(F&& f) noexcept {
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
    template <typename T>
    requires(!std::is_function_v<std::remove_pointer_t<T>>)
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
    // copiable types like int and std::string_view, but the latter is better for expensive-to-move
    // types like std::string. I opted for the former so that this is cheap when doing cheap things
    // and because you can always pass expensive-to-move types by reference if you want to, but if
    // we added a reference here, you couldn't remove it.
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
public:
    using result_type = RetType;

    unique_function() = default;

    unique_function(const unique_function&) = delete;
    unique_function& operator=(const unique_function&) = delete;

    unique_function(unique_function&&) noexcept = default;
    unique_function& operator=(unique_function&&) noexcept = default;

    void swap(unique_function& that) noexcept {
        using std::swap;
        swap(_impl, that._impl);
    }

    friend void swap(unique_function& a, unique_function& b) noexcept {
        a.swap(b);
    }

    // TODO: Look into creating a mechanism based upon a unique_ptr to `void *`-like state, and a
    // `void *` accepting function object.  This will permit reusing the core impl object when
    // converting between related function types, such as
    // `int (std::string)` -> `void (const char *)`
    template <typename F>
    requires(!std::same_as<std::decay_t<F>, unique_function> &&
             std::is_invocable_r_v<RetType, F, Args...> && std::move_constructible<F>)
    explicit(false) unique_function(F&& f) : _impl(_makeImpl(std::forward<F>(f))) {}

    explicit(false) unique_function(std::nullptr_t) noexcept {}

    RetType operator()(Args... args) const {
        invariant(static_cast<bool>(*this));
        return _impl->call(std::forward<Args>(args)...);
    }

    explicit operator bool() const noexcept {
        return static_cast<bool>(_impl);
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

    bool operator==(std::nullptr_t) const noexcept {
        return !*this;
    }

private:
    struct Impl {
        virtual ~Impl() = default;
        virtual RetType call(Args&&... args) = 0;
    };

    template <typename F>
    static auto _makeImpl(F&& f) {
        struct SpecificImpl : Impl {
            explicit SpecificImpl(F&& f) : f(std::forward<F>(f)) {}

            RetType call(Args&&... args) override {
                if constexpr (std::is_void_v<RetType>) {
                    // Implicitly ignore the return. This avoids issues if func() returns a value,
                    // while ensuring we still get a warning if the value is [[nodiscard]].
                    f(std::forward<Args>(args)...);
                } else {
                    return f(std::forward<Args>(args)...);
                }
            }

            std::decay_t<F> f;
        };

        return std::make_unique<SpecificImpl>(std::forward<F>(f));
    }

    std::unique_ptr<Impl> _impl;
};

namespace [[MONGO_MOD_FILE_PRIVATE]] functional_details {
/**
 * Helper to pattern-match the signatures for all combinations of const and l-value-qualifed member
 * function pointers. We don't currently support r-value-qualified call operators.
 */
template <typename>
struct UFDeductionHelper {};
template <typename Class, typename Ret, typename... Args>
struct UFDeductionHelper<Ret (Class::*)(Args...)> : std::type_identity<Ret(Args...)> {};
template <typename Class, typename Ret, typename... Args>
struct UFDeductionHelper<Ret (Class::*)(Args...)&> : std::type_identity<Ret(Args...)> {};
template <typename Class, typename Ret, typename... Args>
struct UFDeductionHelper<Ret (Class::*)(Args...) const> : std::type_identity<Ret(Args...)> {};
template <typename Class, typename Ret, typename... Args>
struct UFDeductionHelper<Ret (Class::*)(Args...) const&> : std::type_identity<Ret(Args...)> {};
}  // namespace functional_details

/**
 * Deduction guides for unique_function<Sig> that pluck the signature off of function pointers and
 * non-overloaded, non-generic function objects such as lambdas that don't use `auto` arguments.
 */
template <typename Ret, typename... Args>
unique_function(Ret (*)(Args...)) -> unique_function<Ret(Args...)>;
template <
    typename T,
    typename Sig = typename functional_details::UFDeductionHelper<decltype(&T::operator())>::type>
unique_function(T) -> unique_function<Sig>;

}  // namespace mongo
