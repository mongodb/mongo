///////////////////////////////////////////////////////////////////////////////
// quant_traits.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_STATIC_PRODUCTIONS_QUANT_TRAITS_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_STATIC_PRODUCTIONS_QUANT_TRAITS_HPP_EAN_10_04_2005

#include <boost/mpl/or.hpp>
#include <boost/mpl/bool.hpp>
#include <boost/mpl/integral_c.hpp>
#include <boost/type_traits/is_same.hpp>
#include <boost/xpressive/proto/proto.hpp>
#include <boost/xpressive/detail/detail_fwd.hpp>

#ifdef BOOST_MSVC
# pragma warning(push)
# pragma warning(disable: 4307) // '+' : integral constant overflow
#endif

namespace boost { namespace xpressive { namespace detail
{

    ///////////////////////////////////////////////////////////////////////////////
    // generic_quant_tag
    template<uint_t Min, uint_t Max>
    struct generic_quant_tag
      : proto::unary_tag
    {
        typedef mpl::integral_c<uint_t, Min> min_type;
        typedef mpl::integral_c<uint_t, Max> max_type;
    };

    ///////////////////////////////////////////////////////////////////////////////
    // min_type / max_type
    template<typename Tag>
    struct min_type : Tag::min_type {};

    template<>
    struct min_type<proto::unary_plus_tag> : mpl::integral_c<uint_t, 1> {};

    template<>
    struct min_type<proto::unary_star_tag> : mpl::integral_c<uint_t, 0> {};

    template<>
    struct min_type<proto::logical_not_tag> : mpl::integral_c<uint_t, 0> {};

    template<typename Tag>
    struct max_type : Tag::max_type {};

    template<>
    struct max_type<proto::unary_plus_tag> : mpl::integral_c<uint_t, UINT_MAX-1> {};

    template<>
    struct max_type<proto::unary_star_tag> : mpl::integral_c<uint_t, UINT_MAX-1> {};

    template<>
    struct max_type<proto::logical_not_tag> : mpl::integral_c<uint_t, 1> {};

    struct use_simple_repeat_predicate
    {
        template<typename Op, typename, typename>
        struct apply
          : use_simple_repeat<typename proto::arg_type<Op>::type>
        {
        };
    };

    ///////////////////////////////////////////////////////////////////////////////
    // is_greedy_quant
    template<typename Xpr>
    struct is_greedy_quant
      : mpl::false_
    {
    };

    template<typename Op, typename Tag>
    struct is_greedy_quant<proto::unary_op<Op, Tag> >
      : mpl::or_
        <
            is_same<Tag, proto::unary_plus_tag>
          , is_same<Tag, proto::unary_star_tag>
          , is_same<Tag, proto::logical_not_tag>
        >
    {
    };

    template<typename Op, uint_t Min, uint_t Max>
    struct is_greedy_quant<proto::unary_op<Op, generic_quant_tag<Min, Max> > >
      : mpl::true_
    {
    };

}}}

#ifdef BOOST_MSVC
# pragma warning(pop)
#endif

#endif
