/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

namespace mongo {
namespace detail {
// Helper for finding the result type of a "callable" object.
template <class T>
struct ResultOf {
    // In the base case, we can find the result of invoking with no args, i.e.,
    // `callable()`.
    // This is the best we can do for types with multiple, or templated operator()
    // e.g., [](auto... foo) {return something(foo...);};
    // As the result type may depend on the argument type.
    // If this is not the desired type, it should be specified directly as the
    // second type param to Deferred.
    using type = std::invoke_result_t<T>;
};

template <class Fn>
requires requires(Fn&& fn) {
    // Restrict to types which _could_ be converted to an std::function.
    // This essentially requires a single unambiguous operator().
    // I.e., not templated, not overloaded.
    std::function(fn);
}
struct ResultOf<Fn> {
    // For non-overloaded/templated functors, and non-generic lambdas we
    // can do better even if it takes arguments - there is a single possible return type,
    // and it is the return type of operator().
    // For simplicity, leverage std::function's deductions here.
    // e.g.,
    //     std::function([](size_t) {return int();})
    //     -> std::function<int(size_t)>
    // std::function is then handled in the specialisation below.
    using type = typename ResultOf<decltype(std::function(std::declval<Fn>()))>::type;
};

template <class T, class... Args>
struct ResultOf<std::function<T(Args...)>> {
    // For std::function, only a single return type is possible.
    using type = T;
};

template <class T>
using result_t = typename ResultOf<T>::type;
}  // namespace detail

/**
 * A template class that provides a way to defer the initialization of an object until its value is
 * actually required. This is also commonly referred to as lazy initialization.
 *
 * Dangers:
 * - This implementation is currently not thread safe, and it shouldn't be used in multi-threaded
 *   fashion.
 * - Be careful about using this for lazy initialization of data members and capturing the 'this'
 *   variable. Code like this will result in buggy/unsafe move constructors, which would have a
 *   dangling reference to the moved-from type:
 *
 *   class MyType {
 *       int x;
 *       // !!! Dangling 'this' when moved !!!
 *       Deferred xSquared{[this]() { return this->x * this-> x; };
 *   };
 *   Instead, it is better to do something like this:
 *   class MyType {
 *       int xSquared() const {
 *          return *_xSquared.get(_x);
 *       }
 *
 *       int _x;
 *       Deferred _xSquared{[](int x) { return x * x; };
 *   };
 * - As a similar danger, the value is only computed once. if you initialize it with arguments like
 *   the above 'xSquared()' implementation, then be cogniscent that the value will never change. If
 *   '_x' changes, '_xSquared' will not.
 *
 * A Deferred class can be constructed with either an initial value (eager initialization) or a
 * function that will generate the value when needed.
 *
 * @tparam InitializerType Type of the "callable" object to invoke to generate a result.
 * @tparam ResultType Type of the result to generate on demand (deduced if possible).
 */
template <class InitializerType, class ResultType = detail::result_t<InitializerType>>
class Deferred {
public:
    using T = ResultType;

    Deferred() = default;
    /**
     * Instantiates a Deferred<T> with the given data - no callbacks or lazy initialization.
     *
     * Note that when initialised with a value eagerly, Deferred cannot deduce the initialiser type;
     * it will need explicitly providing;
     *
     *  Deferred<int(*)()>(1);
     *
     * Trying to do anything smarter here would:
     * A) Force some default initializer type on users - a function pointer would probably be
     * acceptable. B) Cause frustrating issues with higher order functions. C) Obfuscate issues if a
     * caller accidentally provides something _not_ callable as an initialiser if it gets assumed to
     * be an "eager" value.
     *
     */
    Deferred(T data) {
        new (&_data._value) T(std::move(data));
        _data._isInitialized = true;
    }

    static constexpr bool move_constructible =
        std::is_move_constructible_v<InitializerType> && std::is_move_constructible_v<ResultType>;

    static constexpr bool move_assignable =
        std::is_move_assignable_v<InitializerType> && std::is_move_assignable_v<ResultType>;

    // It is _unlikely_ that copying a Deferred is what a user really wanted; that's likely
    // to lead to wasteful repeated effort.
    Deferred(const Deferred& other) = delete;

    // Move construction requires both the initialiser and cached value type to be
    // move constructible.
    Deferred(Deferred&& other) requires move_constructible
        : _initializer(std::move(other._initializer)) {
        if (other.isInitialized()) {
            new (&_data._value) T(std::move(other._data._value));
        }

        // Note! The moved-out-of object still needs to call a destructor, so
        // other._data._isInitialized cannot be cleared here.
        _data._isInitialized = other._data._isInitialized;
    }

    Deferred& operator=(const Deferred& other) = delete;

    Deferred& operator=(Deferred&& other) requires move_assignable {
        std::swap(_initializer, other._initializer);
        if (other.isInitialized()) {
            if (isInitialized()) {
                // Both existing and other values have been constructed;
                // move assignment is okay.
                _data._value = std::move(other._data._value);
            } else {
                // This Deferred has not been initialised yet but the other has; a constructor
                // must be called before it is correct to use _data._value, so move construct here.
                new (&_data._value) T(std::move(other._data._value));
            }
        }
        // Note! The moved-out-of object still needs to call a destructor, so
        // other._data._isInitialized cannot be cleared here.
        _data._isInitialized = other._data._isInitialized;
        return *this;
    }

    /**
     * Stores a function to compute a T later. Please note the warnings described in this class
     * comment.
     */
    Deferred(InitializerType initializer) : _initializer(std::move(initializer)) {}

    /**
     * Returns a reference to the managed object. Initializes the object if it hasn't done so
     * already.
     */
    T& get(auto&&... args) const {
        if (!_data._isInitialized) {
            new (&_data._value) T(_initializer(std::forward<decltype(args)>(args)...));
            _data._isInitialized = true;
        }
        return _data._value;
    }

    const T& operator()(auto&&... args) const {
        return get(std::forward<decltype(args)>(args)...);
    }

    /**
     * Dereferences the pointer to the managed object. Note this is only a valid shortcut if there
     * are no arguments to '_initializer'.
     */
    T* operator->() const {
        return &get();
    }

    /**
     * Returns a referenced to the managed object. Initializes the object if it hasn't done so
     * already. Note this is only a valid shortcut if there are no arguments to '_initializer'.
     */
    T& operator*() const {
        return get();
    }

    bool isInitialized() const {
        return _data._isInitialized;
    }

private:
    struct Storage {
        // User-defined default constructor which intentionally does not initialise _value.
        // Note that this cannot be defaulted; if it is it may still be implicitly _deleted_
        // as ResultType may not be trivial, failing compilation.
        Storage() {}
        ~Storage() {
            if (_isInitialized) {
                _value.~T();
            }
        }

        // Value wrapped in union to allow it to remain uninitialised until first use.
        // As a result, copy/move assignment is not required.
        // Further, in cases with elision, e.g.,
        // Deferred([]{return Foobar("a", "b", 3);};
        // the contained type does not even need to be copy/move constructible.
        union {
            T _value;
        };
        bool _isInitialized = false;
    };
    mutable Storage _data;
    InitializerType _initializer;
};

/**
 * Shorthand for:
 *     Deferred<std::function<Ret(Args1, Args2, ...)>>
 *
 * This intentionally opts-in to the overhead of std::function.
 *
 * Use of std::function type-erasure is only beneficial if the same type needs to store potentially
 * different functors internally - e.g., when taken as a non-templated argument, when stored as a
 * member containing with one of multiple functions.
 *
 * Where performance is of concern, prefer to use Deferred wrapping the specific callable type
 * you are using, e.g., with deduction
 *
 *  auto fn = Deferred([](){ <some expensive computation> });
 *
 * Note: deduction for alias templates does exist, see:
 *     https://en.cppreference.com/w/cpp/language/class_template_argument_deduction#Deduction_for_alias_templates
 * but is not yet well supported. As such, the following will not currently compile:
 *
 *  auto fn = DeferredFn([](){ <some expensive computation> });
 *
 * This will instead currently fail with:
 *
 * "alias template 'DeferredFn' requires template arguments; argument deduction only allowed for
 * class templates"
 *
 * An alternative would be to rely on std::function's deduction:
 *
 *  auto fn = Deferred(std::function([](){ <some expensive computation> }));
 *
 * Of this alias could be re-written as an inheriting struct.
 */
template <class Ret, class... Args>
using DeferredFn = Deferred<std::function<Ret(Args...)>>;

/**
 * Helper for when the Initializer type can be deduced, but the
 * return type cannot - e.g.,
 *
 * Deferred([&](auto a){return a+b;});
 *
 * Here the stored type depends on the argument type, and cannot be
 * deduced.
 *
 * Naming the arguments for this would be a pain:
 *
 *     Deferred<???, size_t>(...)
 *
 * Note that the capture means this lambda cannot decay to a function ptr,
 * otherwise one could use, e.g.,:
 *
 *     Deferred<size_t(int)>
 *
 * For that case, use:
 *
 *     deferred<size_t>([&](){...});
 */

template <class T>
auto deferred(auto&& function) {
    return Deferred<decltype(function), T>(std::forward<decltype(function)>(function));
}

}  // namespace mongo
