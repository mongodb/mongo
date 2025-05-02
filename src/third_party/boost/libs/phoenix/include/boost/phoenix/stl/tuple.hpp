/*==============================================================================
    Copyright (c) 2005-2008 Hartmut Kaiser
    Copyright (c) 2005-2010 Joel de Guzman
    Copyright (c) 2010 Thomas Heller
    Copyright (c) 2021 Beojan Stanislaus

    Distributed under the Boost Software License, Version 1.0. (See accompanying
    file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/

#ifndef BOOST_PHOENIX_STL_TUPLE_H_
#define BOOST_PHOENIX_STL_TUPLE_H_

#if __cplusplus >= 201402L                                                     \
  || (defined(_MSC_FULL_VER) && _MSC_FULL_VER >= 190024210)

#include <tuple>
#include <boost/phoenix/core/argument.hpp>
#include <boost/phoenix/core/call.hpp>
#include <boost/phoenix/core/expression.hpp>
#include <boost/phoenix/core/limits.hpp>
#include <boost/phoenix/core/meta_grammar.hpp>

// Lazy functions for std::tuple (and similar)
// get will work wherever it is accessible through ADL
// Cribbing from functions in object

namespace boost { namespace phoenix { namespace tuple_detail
{
    // Wrappers to pass a type or an index
    template<typename T>
    struct type_wrap
    {
        typedef T type;
    };
    template<int N>
    struct idx_wrap
    {
        static constexpr int idx = N;
    };
}}} // namespace boost::phoenix::tuple_detail

BOOST_PHOENIX_DEFINE_EXPRESSION(
  (boost)(phoenix)(get_with_type),
  (proto::terminal<tuple_detail::type_wrap<proto::_> >)(meta_grammar))

BOOST_PHOENIX_DEFINE_EXPRESSION(
  (boost)(phoenix)(get_with_idx),
  (proto::terminal<proto::_>)(meta_grammar))

namespace boost { namespace phoenix {
    namespace impl {
        struct get_with_type
        {
            // Don't need to use result_of protocol since this only works with C++11+
            // anyway
            template<typename T, typename Expr, typename Context>
            auto& operator()(T, const Expr& t, const Context& ctx) const
            {
                using std::get; // Prevents the next line from being a syntax error <
                                // C++20
                using T_ = typename proto::result_of::value<T>::type;
                return get<typename T_::type>(boost::phoenix::eval(t, ctx));
            }
        };

        struct get_with_idx
        {
            // Don't need to use result_of protocol since this only works with C++11+
            // anyway
            template<typename T, typename Expr, typename Context>
            auto& operator()(T, const Expr& t, const Context& ctx) const
            {
                using std::get; // Prevents the next line from being a syntax error <
                                // C++20
                using T_ = typename proto::result_of::value<T>::type;
                return get<T_::idx>(boost::phoenix::eval(t, ctx));
            }
        };
    }

    template<typename Dummy>
    struct default_actions::when<rule::get_with_type, Dummy>
        : call<impl::get_with_type, Dummy>
    {};
    template<typename Dummy>
    struct default_actions::when<rule::get_with_idx, Dummy>
        : call<impl::get_with_idx, Dummy>
    {};

    template<typename T, typename Tuple>
    inline typename expression::get_with_type<tuple_detail::type_wrap<T>, Tuple>::
    type const
    get_(const Tuple& t)
    {
        return expression::get_with_type<tuple_detail::type_wrap<T>, Tuple>::make(
        tuple_detail::type_wrap<T>(), t);
    }

    template<int N, typename Tuple>
    inline typename expression::get_with_idx<tuple_detail::idx_wrap<N>, Tuple>::
    type const
    get_(const Tuple& t)
    {
        return expression::get_with_idx<tuple_detail::idx_wrap<N>, Tuple>::make(
        tuple_detail::idx_wrap<N>(), t);
    }

#if 0 // Disabled this for now due to ODR viaolations $$$ Fix Me $$$
    // Make unpacked argument placeholders
    namespace placeholders {
        #define BOOST_PP_LOCAL_LIMITS (1, BOOST_PHOENIX_ARG_LIMIT)
        #define BOOST_PP_LOCAL_MACRO(N)                                                 \
            const auto uarg##N =                                                        \
            boost::phoenix::get_<(N)-1>(boost::phoenix::placeholders::arg1);
        #include BOOST_PP_LOCAL_ITERATE()
    }
#endif

}} // namespace boost::phoenix

#endif // C++ 14
#endif // BOOST_PHOENIX_STL_TUPLE_H_
