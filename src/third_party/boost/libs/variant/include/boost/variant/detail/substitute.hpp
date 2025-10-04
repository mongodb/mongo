//-----------------------------------------------------------------------------
// boost variant/detail/substitute.hpp header file
// See http://www.boost.org for updates, documentation, and revision history.
//-----------------------------------------------------------------------------
//
// Copyright (c) 2003
// Eric Friedman
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_VARIANT_DETAIL_SUBSTITUTE_HPP
#define BOOST_VARIANT_DETAIL_SUBSTITUTE_HPP

#include <boost/mpl/aux_/config/ctps.hpp>

#include <boost/variant/detail/substitute_fwd.hpp>
#include <boost/variant/variant_fwd.hpp>
#include <boost/mpl/aux_/lambda_arity_param.hpp>
#include <boost/mpl/aux_/preprocessor/params.hpp>
#include <boost/mpl/aux_/preprocessor/repeat.hpp>
#include <boost/mpl/int_fwd.hpp>
#include <boost/mpl/limits/arity.hpp>
#include <boost/preprocessor/empty.hpp>

namespace boost {
namespace detail { namespace variant {

///////////////////////////////////////////////////////////////////////////////
// (detail) metafunction substitute
//
// Substitutes one type for another in the given type expression.
//

//
// primary template
//
template <
      typename T, typename Dest, typename Source
      BOOST_MPL_AUX_LAMBDA_ARITY_PARAM(
          typename Arity /* = ... (see substitute_fwd.hpp) */
        )
    >
struct substitute
{
    typedef T type;
};

//
// tag substitution specializations
//

#define BOOST_VARIANT_AUX_ENABLE_RECURSIVE_IMPL_SUBSTITUTE_TAG(CV_) \
    template <typename Dest, typename Source> \
    struct substitute< \
          CV_ Source \
        , Dest \
        , Source \
          BOOST_MPL_AUX_LAMBDA_ARITY_PARAM(mpl::int_<-1>) \
        > \
    { \
        typedef CV_ Dest type; \
    }; \
    /**/

BOOST_VARIANT_AUX_ENABLE_RECURSIVE_IMPL_SUBSTITUTE_TAG( BOOST_PP_EMPTY() )
BOOST_VARIANT_AUX_ENABLE_RECURSIVE_IMPL_SUBSTITUTE_TAG(const)
BOOST_VARIANT_AUX_ENABLE_RECURSIVE_IMPL_SUBSTITUTE_TAG(volatile)
BOOST_VARIANT_AUX_ENABLE_RECURSIVE_IMPL_SUBSTITUTE_TAG(const volatile)

#undef BOOST_VARIANT_AUX_ENABLE_RECURSIVE_IMPL_SUBSTITUTE_TAG

//
// pointer specializations
//
#define BOOST_VARIANT_AUX_ENABLE_RECURSIVE_IMPL_HANDLE_POINTER(CV_) \
    template <typename T, typename Dest, typename Source> \
    struct substitute< \
          T * CV_ \
        , Dest \
        , Source \
          BOOST_MPL_AUX_LAMBDA_ARITY_PARAM(mpl::int_<-1>) \
        > \
    { \
        typedef typename substitute< \
              T, Dest, Source \
            >::type * CV_ type; \
    }; \
    /**/

BOOST_VARIANT_AUX_ENABLE_RECURSIVE_IMPL_HANDLE_POINTER( BOOST_PP_EMPTY() )
BOOST_VARIANT_AUX_ENABLE_RECURSIVE_IMPL_HANDLE_POINTER(const)
BOOST_VARIANT_AUX_ENABLE_RECURSIVE_IMPL_HANDLE_POINTER(volatile)
BOOST_VARIANT_AUX_ENABLE_RECURSIVE_IMPL_HANDLE_POINTER(const volatile)

#undef BOOST_VARIANT_AUX_ENABLE_RECURSIVE_IMPL_HANDLE_POINTER

//
// reference specializations
//
template <typename T, typename Dest, typename Source>
struct substitute<
      T&
    , Dest
    , Source
      BOOST_MPL_AUX_LAMBDA_ARITY_PARAM(mpl::int_<-1>)
    >
{
    typedef typename substitute<
          T, Dest, Source
        >::type & type;
};

//
// template expression (i.e., F<...>) specializations
//

template <
      template <typename...> class F
    , typename... Ts
    , typename Dest
    , typename Source
      BOOST_MPL_AUX_LAMBDA_ARITY_PARAM(typename Arity)
    >
struct substitute<
      F<Ts...>
    , Dest
    , Source
      BOOST_MPL_AUX_LAMBDA_ARITY_PARAM(Arity)
    >
{
    typedef F<typename substitute<
          Ts, Dest, Source
        >::type...> type;
};

//
// function specializations
//
template <
      typename R
    , typename... A
    , typename Dest
    , typename Source
    >
struct substitute<
      R (*)(A...)
    , Dest
    , Source
      BOOST_MPL_AUX_LAMBDA_ARITY_PARAM(mpl::int_<-1>)
    >
{
private:
    typedef typename substitute< R, Dest, Source >::type r;

public:
    typedef r (*type)(typename substitute<
          A, Dest, Source
        >::type...);
};

}} // namespace detail::variant
} // namespace boost

#endif // BOOST_VARIANT_DETAIL_SUBSTITUTE_HPP
