/*=============================================================================
    Copyright (c) 2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ITERATOR_DEREF_HPP)
#define FUSION_ITERATOR_DEREF_HPP

#include <boost/spirit/fusion/detail/config.hpp>
#include <boost/spirit/fusion/iterator/detail/iterator_base.hpp>
#include <boost/spirit/fusion/iterator/as_fusion_iterator.hpp>
#include <boost/utility/enable_if.hpp>
#include <boost/type_traits/is_const.hpp>

namespace boost { namespace fusion
{
    namespace meta
    {
        template <typename Tag>
        struct deref_impl
        {
            template <typename Iterator>
            struct apply {};
        };

        template <typename Iterator>
        struct deref
        {
            typedef as_fusion_iterator<Iterator> converter;
            typedef typename converter::type iter;

            typedef typename
                deref_impl<FUSION_GET_TAG(iter)>::
                    template apply<iter>::type
            type;
        };
    }

    namespace deref_detail {
        template <typename Iterator>
        typename meta::deref<Iterator>::type
        deref(Iterator const& i,mpl::true_)
        {
            typedef as_fusion_iterator<Iterator> converter;
            typedef typename converter::type iter;

            typename meta::deref<iter>::type result =
                meta::deref_impl<FUSION_GET_TAG(iter)>::
                    template apply<iter>::call(converter::convert(i));
            return result;
        }

        template <typename Iterator>
        inline typename meta::deref<Iterator>::type
        deref(Iterator& i,mpl::false_)
        {
            typedef as_fusion_iterator<Iterator> converter;
            typedef typename converter::type iter;

            typename meta::deref<iter>::type result =
                meta::deref_impl<FUSION_GET_TAG(iter)>::
                    template apply<iter>::call(converter::convert(i));
            return result;
        }
    }

    template <typename Iterator>
    typename meta::deref<Iterator>::type
    deref(Iterator& i) {
        return deref_detail::deref(i,is_const<Iterator>());
    }

    template <typename Iterator>
    typename meta::deref<Iterator>::type
    deref(Iterator const & i) {
        return deref_detail::deref(i,is_const<Iterator const>());
    }

    template <typename Iterator>
    typename meta::deref<Iterator>::type
    operator*(iterator_base<Iterator> const& i)
    {
        return fusion::deref(i.cast());
    }

    template <typename Iterator>
    inline typename meta::deref<Iterator>::type
    operator*(iterator_base<Iterator>& i)
    {
        return fusion::deref(i.cast());
    }

    // Note: VC7.1 has a problem when we pass the return value directly.
    // Try removing the named temporary. This only happens on debug builds.
    // It seems to be a return value optimization bug.
}}

#endif
