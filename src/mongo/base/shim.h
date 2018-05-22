/**
 *    Copyright (C) 2018 MongoDB Inc.
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

#include <functional>

#include "mongo/base/init.h"
#include "mongo/config.h"

/**
 * The `SHIM` mechanism allows for the creation of "weak-symbol-like" functions which can have their
 * actual implementation injected in the final binary without creating a link dependency upon any
 * actual implementation.  One uses it like this:
 *
 * In a header:
 * ```
 * class MyClass {
 *   public:
 *     static MONGO_DECLARE_SHIM((int)->std::string) helloWorldFunction;
 * };
 * ```
 *
 * In the corresponding C++ file (which is a link dependency):
 * ```
 * MONGO_DEFINE_SHIM(MyClass::helloWorldFunction);
 * ```
 *
 * And in any number of implementation files:
 * ```
 * MONGO_REGISTER_SHIM(MyClass::helloWorldFunction)(int value)->std::string {
 *     if (value == 42) {
 *         return "Hello World";
 *     } else {
 *         return "No way!";
 *     }
 * }
 * ```
 *
 * This can be useful for making auto-registering and auto-constructing mock and release class
 * factories, among other useful things
 */

namespace mongo {
template <typename T>
struct PrivateCall;

/**
 * When declaring shim functions that should be private, they really need to be public; however,
 * this type can be used as a parameter to permit the function to only be called by the type
 * specified in the template parameter.
 */
template <typename T>
struct PrivateTo {
private:
    friend PrivateCall<T>;

    PrivateTo() = default;
};

/**
 * When calling shim functions that should be private, you pass an immediately created instance of
 * the type `PrivateCall< T >`, where `T` is the type that `PrivateTo` requires as a template
 * parameter.
 */
template <typename T>
struct PrivateCall {
private:
    friend T;
    PrivateCall() {}

public:
    operator PrivateTo<T>() {
        return {};
    }
};
}  // namespace mongo

namespace shim_detail {
/**
 * This type, `storage`, is used as a workaround for needing C++17 `inline` variables.  The template
 * static member is effectively `inline` already.
 */
template <typename T, typename tag = void>
struct storage {
    static T data;
};

template <typename T, typename tag>
T storage<T, tag>::data = {};
}  // namespace shim_detail

#define MONGO_SHIM_DEPENDENTS ("ShimHooks")

namespace mongo {
#ifdef MONGO_CONFIG_CHECK_SHIM_DEPENDENCIES
const bool checkShimsViaTUHook = true;
#define MONGO_SHIM_TU_HOOK(name) \
    name {}
#else
const bool checkShimsViaTUHook = false;
#define MONGO_SHIM_TU_HOOK(name)
#endif
}  // namespace mongo

/**
 * Declare a shimmable function with signature `SHIM_SIGNATURE`.  Declare such constructs in a C++
 * header as static members of a class.
 */
#define MONGO_DECLARE_SHIM(/*SHIM_SIGNATURE*/...) MONGO_DECLARE_SHIM_1(__LINE__, __VA_ARGS__)
#define MONGO_DECLARE_SHIM_1(LN, ...) MONGO_DECLARE_SHIM_2(LN, __VA_ARGS__)
#define MONGO_DECLARE_SHIM_2(LN, ...)                                                             \
    const struct ShimBasis_##LN {                                                                 \
        ShimBasis_##LN() = default;                                                               \
        struct MongoShimImplGuts {                                                                \
            template <bool required = mongo::checkShimsViaTUHook>                                 \
            struct AbiCheckType {                                                                 \
                AbiCheckType() = default;                                                         \
            };                                                                                    \
            using AbiCheck = AbiCheckType<>;                                                      \
            struct LibTUHookTypeBase {                                                            \
                LibTUHookTypeBase();                                                              \
            };                                                                                    \
            template <bool required = true>                                                       \
            struct LibTUHookType : LibTUHookTypeBase {};                                          \
            using LibTUHook = LibTUHookType<>;                                                    \
            struct ImplTUHookTypeBase {                                                           \
                ImplTUHookTypeBase();                                                             \
            };                                                                                    \
            template <bool required = mongo::checkShimsViaTUHook>                                 \
            struct ImplTUHookType : ImplTUHookTypeBase {};                                        \
            using ImplTUHook = ImplTUHookType<>;                                                  \
                                                                                                  \
            static auto functionTypeHelper __VA_ARGS__;                                           \
            /* Workaround for Microsoft -- by taking the address of this function pointer, we     \
             * avoid the problems that their compiler has with default * arguments in deduced     \
             * typedefs. */                                                                       \
            using function_type_pointer = decltype(&MongoShimImplGuts::functionTypeHelper);       \
            using function_type = std::remove_pointer_t<function_type_pointer>;                   \
            MongoShimImplGuts* abi(const AbiCheck* const) {                                       \
                return this;                                                                      \
            }                                                                                     \
            MongoShimImplGuts* lib(const LibTUHook* const) {                                      \
                LibTUHook{};                                                                      \
                return this;                                                                      \
            }                                                                                     \
            MongoShimImplGuts* impl(const ImplTUHook* const) {                                    \
                MONGO_SHIM_TU_HOOK(ImplTUHook);                                                   \
                return this;                                                                      \
            }                                                                                     \
            virtual auto implementation __VA_ARGS__ = 0;                                          \
                                                                                                  \
            using tag =                                                                           \
                std::tuple<MongoShimImplGuts::function_type, AbiCheck, LibTUHook, ImplTUHook>;    \
        };                                                                                        \
                                                                                                  \
        using storage = shim_detail::storage<MongoShimImplGuts*, MongoShimImplGuts::tag>;         \
                                                                                                  \
        /* TODO: When the dependency graph is fixed, add the `impl()->` call to the call chain */ \
        template <typename... Args>                                                               \
        auto operator()(Args&&... args) const                                                     \
            noexcept(noexcept(storage::data->abi(nullptr)->lib(nullptr)->implementation(          \
                std::forward<Args>(args)...)))                                                    \
                -> decltype(storage::data->abi(nullptr)->lib(nullptr)->implementation(            \
                    std::forward<Args>(args)...)) {                                               \
            return storage::data->abi(nullptr)->lib(nullptr)->implementation(                     \
                std::forward<Args>(args)...);                                                     \
        }                                                                                         \
    }

/**
 * Define a shimmable function with name `SHIM_NAME`, returning a value of type `RETURN_TYPE`, with
 * any arguments.  This shim definition macro should go in the associated C++ file to the header
 * where a SHIM was defined.  This macro does not emit a function definition, only the customization
 * point's machinery.
 */
#define MONGO_DEFINE_SHIM(/*SHIM_NAME*/...) MONGO_DEFINE_SHIM_1(__LINE__, __VA_ARGS__)
#define MONGO_DEFINE_SHIM_1(LN, ...) MONGO_DEFINE_SHIM_2(LN, __VA_ARGS__)
#define MONGO_DEFINE_SHIM_2(LN, ...)                                                          \
    namespace {                                                                               \
    namespace shim_namespace##LN {                                                            \
        using ShimType = decltype(__VA_ARGS__);                                               \
    } /*namespace shim_namespace*/                                                            \
    } /*namespace*/                                                                           \
    shim_namespace##LN::ShimType::MongoShimImplGuts::LibTUHookTypeBase::LibTUHookTypeBase() = \
        default;                                                                              \
    shim_namespace##LN::ShimType __VA_ARGS__{};

#define MONGO_SHIM_EVIL_STRINGIFY_(args) #args


/**
 * Define an implementation of a shimmable function with name `SHIM_NAME`.  The compiler will check
 * supplied parameters for correctness.  This shim registration macro should go in the associated
 * C++ implementation file to the header where a SHIM was defined.   Such a file would be a mock
 * implementation or a real implementation, for example
 */
#define MONGO_REGISTER_SHIM(/*SHIM_NAME*/...) MONGO_REGISTER_SHIM_1(__LINE__, __VA_ARGS__)
#define MONGO_REGISTER_SHIM_1(LN, ...) MONGO_REGISTER_SHIM_2(LN, __VA_ARGS__)
#define MONGO_REGISTER_SHIM_2(LN, ...)                                                          \
    namespace {                                                                                 \
    namespace shim_namespace##LN {                                                              \
        using ShimType = decltype(__VA_ARGS__);                                                 \
                                                                                                \
        class Implementation final : public ShimType::MongoShimImplGuts {                       \
            /* Some compilers don't work well with the trailing `override` in this kind of      \
             * function declaration. */                                                         \
            ShimType::MongoShimImplGuts::function_type implementation; /* override */           \
        };                                                                                      \
                                                                                                \
        ::mongo::Status createInitializerRegistration(::mongo::InitializerContext* const) {     \
            static Implementation impl;                                                         \
            ShimType::storage::data = &impl;                                                    \
            return Status::OK();                                                                \
        }                                                                                       \
                                                                                                \
        const ::mongo::GlobalInitializerRegisterer registrationHook{                            \
            std::string(MONGO_SHIM_EVIL_STRINGIFY_(__VA_ARGS__)),                               \
            {},                                                                                 \
            {MONGO_SHIM_DEPENDENTS},                                                            \
            mongo::InitializerFunction(createInitializerRegistration)};                         \
    } /*namespace shim_namespace*/                                                              \
    } /*namespace*/                                                                             \
                                                                                                \
    shim_namespace##LN::ShimType::MongoShimImplGuts::ImplTUHookTypeBase::ImplTUHookTypeBase() = \
        default;                                                                                \
                                                                                                \
    auto shim_namespace##LN::Implementation::implementation /* After this point someone just    \
                                                               writes the signature's arguments \
                                                               and return value (using arrow    \
                                                               notation).  Then they write the  \
                                                               body. */
