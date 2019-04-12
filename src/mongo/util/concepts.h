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

#include <type_traits>

#if defined(__cpp_concepts)

//
// These macros provide an emulation of C++20-style requires-clauses and requires-expression. These
// are part of the "Concepts" feature, however this does not provide a way to emulate the definition
// of actual concepts. You will need to continue either using the std type_traits, or define your
// own constexpr bools.
//
// The emulation does allow overloading based on requirements, however because it doesn't have a way
// to define actual concepts, there is no subsumption/refinement rules. In other words, you are
// responsible for ensuring that only one overload (with the same arguments) will match a given
// call, the compiler won't select the best one. eg, with real concepts support, you can overload
// these functions and it will do the right thing:
//      template <typename T>                                void doThing(T);
//      template <typename T> requires(IsFoo<T>)             void doThing(T);
//      template <typename T> requires(IsFoo<T> && IsBar<T>) void doThing(T);
//
// With the emulation, you need to explicitly make them all mutually exclusive:
//      template <typename T> requires(!IsFoo<T>)              void doThing(T);
//      template <typename T> requires( IsFoo<T> && !IsBar<T>) void doThing(T);
//      template <typename T> requires( IsFoo<T> &&  IsBar<T>) void doThing(T);
//

/**
 * Use "TEMPLATE(typename T)" instead of "template <typename T>" when you are using the REQUIRES
 * macros.
 */
#define TEMPLATE(...) template <__VA_ARGS__>

/**
 * Disables this template if the argument evaluates to false at compile time.
 *
 * Use the OUT_OF_LINE_DEF version when you are defining the template out of line following an
 * earlier forward declaration.
 *
 * Must be placed between the TEMPLATE() macro and the declaration (ie it doesn't support the
 * "trailing requires clause" style). Can not be used to enable/disable explicit specializations,
 * they will need to match exactly one version of the primary template.
 *
 * Be careful will top-level commas, because everything before the comma will be ignored.
 *
 * Example (you could also just define the body with the initial declaration):
 *
 *      TEMPLATE(typename Callback)
 *      REQUIRES(std::is_invocable_v<Callback, Status>)
 *      void registerCallback(Callback&& cb);
 *
 *      TEMPLATE(typename Callback)
 *      REQUIRES_OUT_OF_LINE_DEF(std::is_invocable_v<Callback, Status>)
 *      void registerCallback(Callback&& cb) { stuff }
 */
#define REQUIRES(...) requires(__VA_ARGS__)
#define REQUIRES_OUT_OF_LINE_DEF(...) requires(__VA_ARGS__)

/**
 * Use this on a non-template to impose requirements on it. With *very* few exceptions, this should
 * only be used on non-template methods inside of a class template.
 *
 * Due to limitations of the emulation, you cannot forward declare methods this is used with.
 *
 * Example:
 *      template <typename T>
 *      struct Holder {
 *          REQUIRES_FOR_NON_TEMPLATE(sizeof(T) == 4)
 *          void doThing() {}
 *
 *          REQUIRES_FOR_NON_TEMPLATE(sizeof(T) == 8)
 *          void doThing() {}
 *      };
 */
#define REQUIRES_FOR_NON_TEMPLATE(...) \
    template <int... ignoreThisArg>    \
    requires(__VA_ARGS__)

/**
 * Defines a boolean trait that is true if a set of expressions compiles.
 *
 * Args (some are lists that must be wrapped in parens):
 *      name          - name of the template
 *      (tpl_params)  - template parameters for the trait definition (ie with typename)
 *      (tpl_args)    - template arguments for a usage of the trait (ie without typename)
 *      (decls)       - declarations of variables used in the trait's expression
 *      exprs         - the expressions that are tested to see if they compile
 *
 * Examples (the // separator tends to improve readability with clang-format):
 *      MONGO_MAKE_BOOL_TRAIT(isAddable,
 *                            (typename LHS, typename RHS),
 *                            (LHS, RHS),
 *                            (LHS& mutableLhs, const LHS& lhs, const RHS& rhs),
 *                            //
 *                            mutableLhs += rhs);
 *                            lhs + rhs,
 *
 *      MONGO_MAKE_BOOL_TRAIT(isCallable,
 *                            (typename Func, typename... Args),
 *                            (Func, Args...),
 *                            (Func& func, Args&&... args),
 *                            //
 *                            func(args...));
 *
 * WARNING: This only works for compiler failures in the "immediate context" of the expression.
 *          For example, if a function is templated to take all arguments, but the body will fail to
 *          compile with some, isCallable will either return true or cause a hard compile error.
 *          These should only be used with well-constrained functions.
 *
 * We need to use a real concept to work around https://gcc.gnu.org/bugzilla/show_bug.cgi?id=90287.
 * Still using a constexpr bool in the public API for consistency with the no-concepts
 * implementation so it doesn't behave like a concept during normalization and overload resolution.
 */
#define MONGO_MAKE_BOOL_TRAIT(name, tpl_params, tpl_args, decls, /*exprs*/...) \
    template <MONGO_MAKE_BOOL_TRAIT_HELPER_EXPAND tpl_params>                  \
    MONGO_MAKE_BOOL_TRAIT_CONCEPTS_KEYWORD make_trait_impl_##name##_concept =  \
        requires(MONGO_MAKE_BOOL_TRAIT_HELPER_EXPAND decls) {                  \
        {__VA_ARGS__};                                                         \
    };                                                                         \
    template <MONGO_MAKE_BOOL_TRAIT_HELPER_EXPAND tpl_params>                  \
    constexpr inline bool name =                                               \
        make_trait_impl_##name##_concept<MONGO_MAKE_BOOL_TRAIT_HELPER_EXPAND tpl_args>

// Everything under here is an implementation detail, so you should only need to read it if you are
// looking to change or add something. The whole public API is described above.

#if __cpp_concepts <= 201507  // gcc-8 still uses concepts TS syntax.
#define MONGO_MAKE_BOOL_TRAIT_CONCEPTS_KEYWORD concept bool
#else
#define MONGO_MAKE_BOOL_TRAIT_CONCEPTS_KEYWORD concept
#endif

#else
// This is the polyfill for when not using concepts-enabled compilers.

#define TEMPLATE(...) template <__VA_ARGS__,  // intentionally left open; closed below.

// Note: the best error messages are generated when __VA_ARGS__ is the direct first argument of
// enable_if_t, or directly inside of the parens of a decltype without an extra set of parens.
// If you want to alter these, be sure to check error messages on clang and gcc!
#define REQUIRES(...) std::enable_if_t<(__VA_ARGS__), int> = 0 >

// Same as above, but without the default argument since some compilers (correctly) disallow
// repeating the default argument on the definition.
#define REQUIRES_OUT_OF_LINE_DEF(...) std::enable_if_t<(__VA_ARGS__), int>>

// Need the second arg in the template to depend on both a template argument (so it is dependent),
// and the current line number (so it can be overloaded). The __VA_ARGS__ expression will generally
// be a constant expression at parse time (or class instantiation time), so in order to allow more
// than one overload that is false, we need to defer the evaluation of the outer enable_if_t to
// function instantiation time.
#define REQUIRES_FOR_NON_TEMPLATE(...)        \
    template <int ignoreThisArg = 0,          \
              std::enable_if_t<(__VA_ARGS__), \
                               std::enable_if_t<(ignoreThisArg * __LINE__) == 0, int>> = 0>

// Works by declaring a function template taking `decls` arguments and using expression-SFINAE using
// `exprs` on the return type. A bool is defined as true if it is possible to instantiate the
// template with the supplied arguments by taking its address.
#define MONGO_MAKE_BOOL_TRAIT(name, tpl_params, tpl_args, decls, /*exprs*/...)            \
    template <MONGO_MAKE_BOOL_TRAIT_HELPER_EXPAND tpl_params>                             \
    auto make_trait_impl_##name##_fn(MONGO_MAKE_BOOL_TRAIT_HELPER_EXPAND decls)           \
        ->decltype(__VA_ARGS__);                                                          \
                                                                                          \
    template <typename ALWAYS_VOID, MONGO_MAKE_BOOL_TRAIT_HELPER_EXPAND tpl_params>       \
    constexpr inline bool make_trait_impl_##name##_bool = false;                          \
                                                                                          \
    template <MONGO_MAKE_BOOL_TRAIT_HELPER_EXPAND tpl_params>                             \
    constexpr inline bool make_trait_impl_##name##_bool<                                  \
        std::void_t<decltype(                                                             \
            &make_trait_impl_##name##_fn<MONGO_MAKE_BOOL_TRAIT_HELPER_EXPAND tpl_args>)>, \
        MONGO_MAKE_BOOL_TRAIT_HELPER_EXPAND tpl_args> = true;                             \
                                                                                          \
    template <MONGO_MAKE_BOOL_TRAIT_HELPER_EXPAND tpl_params>                             \
    constexpr inline bool name =                                                          \
        make_trait_impl_##name##_bool<void, MONGO_MAKE_BOOL_TRAIT_HELPER_EXPAND tpl_args>

#endif

// Strips off wrapping parens used to group some arguments. Use *without* any parens.
#define MONGO_MAKE_BOOL_TRAIT_HELPER_EXPAND(...) __VA_ARGS__
