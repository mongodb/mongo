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

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/config.h"     // IWYU pragma: keep
#include "mongo/util/assert_util.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>

/**
 * The `WeakFunction` mechanism allows for the creation of "weak-symbol-like" functions
 * which can have implementations injected into a link target without creating a link
 * dependency. This is used for injecting factory functions and mocks.
 *
 * DEPRECATION WARNING:
 * This library was created as a one-time expediency to resolve technical debt in the
 * link dependency graph. It should not be used in new designs, and existing uses will
 * continue to be phased out over time.
 */
namespace mongo {
class WeakFunctionRegistry {
public:
    class BasicSlot {
    public:
        virtual ~BasicSlot();
        int priority = 0;
    };

    template <typename F>
    class Slot : public BasicSlot {
    public:
        Slot() : Slot(nullptr) {}
        explicit Slot(F* f) : f(f) {}
        F* f;
    };

    /**
     * Get the function slot for `key`. Creating a new empty slot if necessary.
     * The slot thus created is permanently associated with function type `F`.
     * Throws if `key` is not associated with the requested function type `F`.
     */
    template <typename F>
    Slot<F>* getSlot(const std::string& key) {
        auto [iter, ok] = _slots.try_emplace(key, nullptr);
        if (ok) {
            iter->second = std::make_unique<Slot<F>>();
        }
        Slot<F>* slot = dynamic_cast<Slot<F>*>(iter->second.get());
        if (!slot) {
            uasserted(31335, std::string("key ") + key + " mapped to wrong function type");
        }
        return slot;
    }

    /**
     * Make `f` the implementation of function `key`. Subsequent `getSlot<F>(key)` calls
     * will return a slot mapped to a function object that invokes `f` when called.
     *
     * Throws if a previous call with the same `key` and `priority` was made. If keys
     * collide, but at differing priorities, the function that was installed with the
     * greater priority gets the slot.
     */
    template <typename F>
    void inject(const std::string& key, F* impl, int priority) {
        Slot<F>* slot = getSlot<F>(key);
        if (slot->priority > priority)
            return;
        if (slot->priority == priority && slot->f)
            uasserted(31336, std::string("key collision: ") + key);
        slot->priority = priority;
        slot->f = impl;
    }

private:
    std::map<std::string, std::unique_ptr<BasicSlot>> _slots;
};

WeakFunctionRegistry& globalWeakFunctionRegistry();

template <typename F>
class WeakFunction {
public:
    explicit WeakFunction(std::string key)
        : _key(std::move(key)), _slot(globalWeakFunctionRegistry().getSlot<F>(_key)) {}

    template <typename... A>
    decltype(auto) operator()(A&&... a) const {
        return std::invoke(_slot->f, std::forward<A>(a)...);
    }

private:
    std::string _key;
    const WeakFunctionRegistry::Slot<F>* _slot;
};

/**
 * Associates an implementation function with a name in the global WeakFunction registry.
 * A registration object, useful only for its constructor's side effects.
 *
 * Example:
 *
 *   // Inject an implementation of the WeakFunction "badSqrt".
 *   static double badSqrtImpl(double x) {
 *     return std::sqrt(x) + 1;
 *   }
 *   static auto sqrtRegistration = WeakFunctionRegistration("badSqrt", badSqrtImpl);
 *
 *   // Elsewhere...
 *   double badSqrt(double x) {
 *     // Use a WeakFunction to allow injected implementations of badSqrt.
 *     static auto weak = WeakFunction<double(double)>("badSqrt");
 *     return weak(x);
 *   }
 *
 * The macros below help with the syntax a bit. The example can be updated to use the
 * MONGO_WEAK_FUNCTION_ macros.
 *
 *   static double badSqrtImpl(double x) {
 *     return std::sqrt(x) + 1;
 *   }
 *   static auto sqrtRegistration = MONGO_WEAK_FUNCTION_REGISTRATION(badSqrt, badSqrtImpl);
 *
 *   // Elsewhere...
 *   double badSqrt(double x) {
 *     // Use a WeakFunction to allow injected implementations of badSqrt.
 *     // Notice that the function type of `double(double)` is implicitly determined.
 *     static auto weak = MONGO_WEAK_FUNCTION_DEFINITION(badSqrt);
 *     return weak(x);
 *   }
 */
template <typename F>
struct WeakFunctionRegistration {
    /**
     * Injects `f` as the implementation for the WeakFunction name `key` in the global
     * registry. A priority can optionally be specified as an int parameter. Default
     * priority is 0.
     */
    WeakFunctionRegistration(std::string key, F* impl, int priority = 0) {
        globalWeakFunctionRegistry().inject<F>(key, impl, priority);
    }
};

/**
 * Wrapper for the WeakFunctionRegistration constructor call.
 * Declares a registration object that registers the function `impl` as the implementation
 * of any WeakFunction objects mapped to the key `func`.
 * See WeakFunctionRegistration documentation for an example.
 */
#define MONGO_WEAK_FUNCTION_REGISTRATION_WITH_PRIORITY(func, impl, priority) \
    ::mongo::WeakFunctionRegistration(#func, impl, priority)

/** Usually we don't specify a priority, so this uses default priority 0. */
#define MONGO_WEAK_FUNCTION_REGISTRATION(func, impl) \
    MONGO_WEAK_FUNCTION_REGISTRATION_WITH_PRIORITY(func, impl, 0)

/**
 * Wrapper for the WeakFunction constructor call to create a WeakFunction that agrees with the
 * type signature of the declared function `func`, and uses func's name as a key.
 * See WeakFunctionRegistration documentation for an example.
 */
#define MONGO_WEAK_FUNCTION_DEFINITION(func) ::mongo::WeakFunction<decltype(func)>(#func)

}  // namespace mongo
