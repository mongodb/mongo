/*=============================================================================
    Copyright (c) 2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ITERATOR_EQUAL_TO_HPP)
#define FUSION_ITERATOR_EQUAL_TO_HPP

#include <boost/spirit/fusion/detail/config.hpp>
#include <boost/spirit/fusion/iterator/as_fusion_iterator.hpp>
#include <boost/type_traits/is_same.hpp>
#include <boost/type_traits/add_const.hpp>

namespace boost { namespace fusion
{
    namespace meta
    {
        template <typename Tag>
        struct equal_to_impl
        {
            template <typename I1, typename I2>
            struct apply
            {
                typedef typename
                    is_same<
                        typename add_const<I1>::type
                      , typename add_const<I2>::type
                    >::type
                type;
                BOOST_STATIC_CONSTANT(bool, value = FUSION_GET_VALUE(type));
            };
        };

        template <typename I1, typename I2>
        struct equal_to
            : detail::bool_base<
              typename equal_to_impl<typename as_fusion_iterator<I1>::type::tag>::
                template apply<
                    typename as_fusion_iterator<I1>::type
                  , typename as_fusion_iterator<I2>::type
                >::type
              > {};
    }
}}

#endif

