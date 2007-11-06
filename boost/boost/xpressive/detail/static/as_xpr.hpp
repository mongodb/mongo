///////////////////////////////////////////////////////////////////////////////
// as_xpr.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_STATIC_AS_XPR_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_STATIC_AS_XPR_HPP_EAN_10_04_2005

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <boost/ref.hpp>
#include <boost/mpl/if.hpp>
#include <boost/mpl/assert.hpp>
#include <boost/mpl/identity.hpp>
#include <boost/mpl/apply.hpp>
#include <boost/mpl/placeholders.hpp>
#include <boost/mpl/eval_if.hpp>
#include <boost/mpl/logical.hpp>
#include <boost/type_traits/is_array.hpp>
#include <boost/type_traits/remove_bounds.hpp>
#include <boost/type_traits/remove_pointer.hpp>
#include <boost/type_traits/is_convertible.hpp>
#include <boost/xpressive/detail/detail_fwd.hpp>
#include <boost/xpressive/proto/proto.hpp>
#include <boost/xpressive/detail/static/placeholders.hpp>

namespace boost { namespace xpressive { namespace detail
{

    template<typename T>
    struct wrap { wrap(T); };

    ///////////////////////////////////////////////////////////////////////////////
    // is_string_literal
    //
    template<typename T>
    struct is_string_literal
      : mpl::or_
        <
            is_convertible<T, wrap<char const *> >
          , is_convertible<T, wrap<wchar_t const *> >
        >
    {
    };

    ///////////////////////////////////////////////////////////////////////////////
    // string_generator
    //
    template<typename String>
    struct string_placeholder_generator
    {
        typedef typename remove_cv
        <
            typename mpl::eval_if
            <
                is_array<String>
              , remove_bounds<String>
              , remove_pointer<String>
            >::type
        >::type char_type;

        typedef string_placeholder<char_type> type;
    };

    ///////////////////////////////////////////////////////////////////////////////
    // as_matcher
    template<typename Matcher, bool IsXpr = is_xpr<Matcher>::value>
    struct as_matcher_type
    {
        typedef Matcher type;

        static type const &call(Matcher const &matcher)
        {
            return matcher;
        }
    };

    template<typename Literal>
    struct as_matcher_type<Literal, false>
    {
        typedef typename mpl::eval_if
        <
            is_string_literal<Literal>
          , string_placeholder_generator<Literal>
          , mpl::identity<literal_placeholder<Literal, false> >
        >::type type;

        static type call(Literal const &literal)
        {
            return type(literal);
        }
    };

    template<typename BidiIter>
    struct as_matcher_type<basic_regex<BidiIter>, false>
    {
        typedef regex_placeholder<BidiIter, false> type;

        static type call(basic_regex<BidiIter> const &rex)
        {
            typedef core_access<BidiIter> access;
            shared_ptr<regex_impl<BidiIter> > impl = access::get_regex_impl(rex);
            return type(impl);
        }
    };

    template<typename BidiIter>
    struct as_matcher_type<reference_wrapper<basic_regex<BidiIter> const>, false>
    {
        typedef regex_placeholder<BidiIter, false> type;

        static type call(reference_wrapper<basic_regex<BidiIter> const> const &rex)
        {
            typedef core_access<BidiIter> access;
            shared_ptr<regex_impl<BidiIter> > impl = access::get_regex_impl(rex.get());
            return type(impl);
        }
    };

    ///////////////////////////////////////////////////////////////////////////////
    // as_xpr_type
    //
    template<typename Xpr>
    struct as_xpr_type<Xpr, true> // is_op == true
    {
        typedef Xpr type;
        typedef Xpr const &const_reference;

        static Xpr const &call(Xpr const &xpr)
        {
            return xpr;
        }
    };

    template<typename Xpr>
    struct as_xpr_type<Xpr, false>
    {
        typedef proto::unary_op
        <
            typename as_matcher_type<Xpr>::type
          , proto::noop_tag
        > type;

        typedef type const const_reference;

        static type const call(Xpr const &xpr)
        {
            return proto::noop(detail::as_matcher_type<Xpr>::call(xpr));
        }
    };

}}} // namespace boost::xpressive::detail


namespace boost { namespace xpressive
{

    ///////////////////////////////////////////////////////////////////////////////
    // as_xpr (from a literal to an xpression)
    //
    template<typename Xpr>
    inline typename detail::as_xpr_type<Xpr>::const_reference
    as_xpr(Xpr const &xpr)
    {
        return detail::as_xpr_type<Xpr>::call(xpr);
    }

}} // namespace boost::xpressive

#endif
