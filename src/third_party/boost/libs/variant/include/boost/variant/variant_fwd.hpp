//-----------------------------------------------------------------------------
// boost variant/variant_fwd.hpp header file
// See http://www.boost.org for updates, documentation, and revision history.
//-----------------------------------------------------------------------------
//
// Copyright (c) 2003 Eric Friedman, Itay Maman
// Copyright (c) 2013-2025 Antony Polukhin
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_VARIANT_VARIANT_FWD_HPP
#define BOOST_VARIANT_VARIANT_FWD_HPP

#include <boost/variant/detail/config.hpp>

#include <boost/blank_fwd.hpp>
#include <boost/mpl/arg.hpp>
#include <boost/mpl/limits/arity.hpp>
#include <boost/mpl/aux_/na.hpp>
#include <boost/preprocessor/cat.hpp>
#include <boost/preprocessor/enum.hpp>
#include <boost/preprocessor/enum_params.hpp>
#include <boost/preprocessor/enum_shifted_params.hpp>
#include <boost/preprocessor/repeat.hpp>

///////////////////////////////////////////////////////////////////////////////
// macro BOOST_VARIANT_NO_FULL_RECURSIVE_VARIANT_SUPPORT
//
// Defined if make_recursive_variant cannot be supported as documented.
//
// Note: Currently, MPL lambda facility is used as workaround if defined, and
// so only types declared w/ MPL lambda workarounds will work.
//

#include <boost/variant/detail/substitute_fwd.hpp>

#include <boost/preprocessor/seq/size.hpp>

#define BOOST_VARIANT_CLASS_OR_TYPENAME_TO_SEQ_class class)(
#define BOOST_VARIANT_CLASS_OR_TYPENAME_TO_SEQ_typename typename)(

#define BOOST_VARIANT_CLASS_OR_TYPENAME_TO_VARIADIC_class class...
#define BOOST_VARIANT_CLASS_OR_TYPENAME_TO_VARIADIC_typename typename...

#define ARGS_VARIADER_1(x) x ## N...
#define ARGS_VARIADER_2(x) BOOST_VARIANT_CLASS_OR_TYPENAME_TO_VARIADIC_ ## x ## N

#define BOOST_VARIANT_MAKE_VARIADIC(sequence, x)        BOOST_VARIANT_MAKE_VARIADIC_I(BOOST_PP_SEQ_SIZE(sequence), x)
#define BOOST_VARIANT_MAKE_VARIADIC_I(argscount, x)     BOOST_VARIANT_MAKE_VARIADIC_II(argscount, x)
#define BOOST_VARIANT_MAKE_VARIADIC_II(argscount, orig) ARGS_VARIADER_ ## argscount(orig)

///////////////////////////////////////////////////////////////////////////////
// BOOST_VARIANT_ENUM_PARAMS and BOOST_VARIANT_ENUM_SHIFTED_PARAMS
//
// Convenience macro for enumeration of variant params.
// When variadic templates are available expands:
//      BOOST_VARIANT_ENUM_PARAMS(class Something)      => class Something0, class... SomethingN
//      BOOST_VARIANT_ENUM_PARAMS(typename Something)   => typename Something0, typename... SomethingN
//      BOOST_VARIANT_ENUM_PARAMS(Something)            => Something0, SomethingN...
//      BOOST_VARIANT_ENUM_PARAMS(Something)            => Something0, SomethingN...
//      BOOST_VARIANT_ENUM_SHIFTED_PARAMS(class Something)      => class... SomethingN
//      BOOST_VARIANT_ENUM_SHIFTED_PARAMS(typename Something)   => typename... SomethingN
//      BOOST_VARIANT_ENUM_SHIFTED_PARAMS(Something)            => SomethingN...
//      BOOST_VARIANT_ENUM_SHIFTED_PARAMS(Something)            => SomethingN...
//
// Rationale: Cleaner, simpler code for clients of variant library. Minimal 
// code modifications to move from C++03 to C++11.
//

#define BOOST_VARIANT_ENUM_PARAMS(x) \
    x ## 0, \
    BOOST_VARIANT_MAKE_VARIADIC( (BOOST_VARIANT_CLASS_OR_TYPENAME_TO_SEQ_ ## x), x) \
    /**/

#define BOOST_VARIANT_ENUM_SHIFTED_PARAMS(x) \
    BOOST_VARIANT_MAKE_VARIADIC( (BOOST_VARIANT_CLASS_OR_TYPENAME_TO_SEQ_ ## x), x) \
    /**/


namespace boost {

namespace detail { namespace variant {

///////////////////////////////////////////////////////////////////////////////
// (detail) class void_ and class template convert_void
// 
// Provides the mechanism by which void(NN) types are converted to
// mpl::void_ (and thus can be passed to mpl::list).
//
// Rationale: This is particularly needed for the using-declarations
// workaround (below), but also to avoid associating mpl namespace with
// variant in argument dependent lookups (which used to happen because of
// defaulting of template parameters to mpl::void_).
//

struct void_;

template <typename T>
struct convert_void
{
    typedef T type;
};

template <>
struct convert_void< void_ >
{
    typedef mpl::na type;
};

}} // namespace detail::variant

#define BOOST_VARIANT_AUX_DECLARE_PARAMS BOOST_VARIANT_ENUM_PARAMS(typename T)

///////////////////////////////////////////////////////////////////////////////
// class template variant (concept inspired by Andrei Alexandrescu)
//
// Efficient, type-safe bounded discriminated union.
//
// Preconditions:
//  - Each type must be unique.
//  - No type may be const-qualified.
//
// Proper declaration form:
//   variant<types>    (where types is a type-sequence)
// or
//   variant<T0,T1,...,Tn>  (where T0 is NOT a type-sequence)
//
template < BOOST_VARIANT_AUX_DECLARE_PARAMS > class variant;

///////////////////////////////////////////////////////////////////////////////
// metafunction make_recursive_variant
//
// Exposes a boost::variant with recursive_variant_ tags (below) substituted
// with the variant itself (wrapped as needed with boost::recursive_wrapper).
//
template < BOOST_VARIANT_AUX_DECLARE_PARAMS > struct make_recursive_variant;

#undef BOOST_VARIANT_AUX_DECLARE_PARAMS_IMPL
#undef BOOST_VARIANT_AUX_DECLARE_PARAMS

///////////////////////////////////////////////////////////////////////////////
// type recursive_variant_
//
// Tag type indicates where recursive variant substitution should occur.
//
#if !defined(BOOST_VARIANT_NO_FULL_RECURSIVE_VARIANT_SUPPORT)
    struct recursive_variant_ {};
#else
    typedef mpl::arg<1> recursive_variant_;
#endif

///////////////////////////////////////////////////////////////////////////////
// metafunction make_variant_over
//
// Result is a variant w/ types of the specified type sequence.
//
template <typename Types> struct make_variant_over;

///////////////////////////////////////////////////////////////////////////////
// metafunction make_recursive_variant_over
//
// Result is a recursive variant w/ types of the specified type sequence.
//
template <typename Types> struct make_recursive_variant_over;

} // namespace boost

#endif // BOOST_VARIANT_VARIANT_FWD_HPP
