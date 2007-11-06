/*=============================================================================
    Copyright (c) 2001-2003 Joel de Guzman

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_SEQUENCE_TUPLE50_HPP)
#define FUSION_SEQUENCE_TUPLE50_HPP

#include <boost/spirit/fusion/detail/config.hpp>
#include <boost/spirit/fusion/detail/access.hpp>
#include <boost/spirit/fusion/iterator/tuple_iterator.hpp>
#include <boost/spirit/fusion/sequence/detail/tuple_macro.hpp>
#include <boost/spirit/fusion/sequence/detail/tuple_access_result.hpp>
#include <boost/spirit/fusion/sequence/detail/tuple_value_at_traits.hpp>
#include <boost/spirit/fusion/sequence/detail/tuple_at_traits.hpp>
#include <boost/spirit/fusion/sequence/detail/tuple_size_traits.hpp>
#include <boost/spirit/fusion/sequence/detail/tuple_begin_end_traits.hpp>
#include <boost/spirit/fusion/sequence/detail/sequence_base.hpp>
#include <boost/spirit/fusion/iterator/next.hpp>
#include <boost/mpl/bool.hpp>
#include <boost/mpl/at.hpp>
#include <boost/mpl/int.hpp>
#include <boost/mpl/vector/vector50.hpp>
#include <boost/preprocessor/iteration/iterate.hpp>

#if BOOST_WORKAROUND(BOOST_MSVC, <= 1300)
# include <boost/spirit/fusion/iterator/next.hpp>
#endif

///////////////////////////////////////////////////////////////////////////////
//
//  See tuple10.hpp and detail/tuple10.hpp. The following code are
//  expansions of the tuple_access<N> and tupleN+1 classes for N = 40..50.
//
///////////////////////////////////////////////////////////////////////////////
namespace boost { namespace fusion
{
    namespace detail
    {
        BOOST_PP_REPEAT_FROM_TO(40, 50, FUSION_TUPLE_N_ACCESS, _)
    }

    struct tuple_tag;

#  define BOOST_PP_ITERATION_PARAMS_1 (3, (41, 50, <boost/spirit/fusion/sequence/detail/tuple_body.hpp>))
#  include BOOST_PP_ITERATE()
}}

#endif
