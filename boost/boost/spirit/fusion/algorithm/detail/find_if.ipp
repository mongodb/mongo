/*=============================================================================
    Copyright (c) 2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ALGORITHM_DETAIL_FIND_IF_HPP)
#define FUSION_ALGORITHM_DETAIL_FIND_IF_HPP

#include <boost/mpl/eval_if.hpp>
#include <boost/mpl/apply.hpp>
#include <boost/mpl/or.hpp>
#include <boost/mpl/lambda.hpp>
#include <boost/spirit/fusion/iterator/value_of.hpp>
#include <boost/spirit/fusion/iterator/equal_to.hpp>
#include <boost/spirit/fusion/iterator/next.hpp>

namespace boost { namespace fusion { namespace detail
{
    template <typename Iterator, typename Pred>
    struct apply_filter
    {
        typedef typename
            mpl::apply1<
                Pred
              , typename meta::value_of<Iterator>::type
            >::type
        type;
    };

    template <typename First, typename Last, typename Pred>
    struct main_find_if;

    template <typename First, typename Last, typename Pred>
    struct recursive_find_if
    {
        typedef typename
            main_find_if<
                typename meta::next<First>::type, Last, Pred
            >::type
        type;
    };

    template <typename First, typename Last, typename Pred>
    struct main_find_if
    {
        typedef mpl::or_<
            meta::equal_to<First, Last>
          , apply_filter<First, Pred> >
        filter;

        typedef typename
            mpl::eval_if<
                filter
              , mpl::identity<First>
              , recursive_find_if<First, Last, Pred>
            >::type
        type;
    };

    template <typename First, typename Last, typename Pred>
    struct static_find_if;

    namespace static_find_if_detail {
        template <typename First,typename Last,typename Pred,typename Iterator>
        typename main_find_if<First,Last,typename mpl::lambda<Pred>::type>::type
        call(static_find_if<First,Last,Pred> const& obj,Iterator const& iter);
    }

    template <typename First, typename Last, typename Pred>
    struct static_find_if
    {
        typedef typename
            main_find_if<
                First
              , Last
              , typename mpl::lambda<Pred>::type
            >::type
        type;

        //Workaround to please MSVC
        template<typename Iterator>
        static type
        call(Iterator const& iter)
        {
            return static_find_if_detail::call(static_find_if<First,Last,Pred>(),iter);
        }
    };

    namespace static_find_if_detail {
        template <typename First,typename Last,typename Pred,typename Iterator>
        typename static_find_if<First,Last,Pred>::type
        call(static_find_if<First,Last,Pred> const&, Iterator const& iter, mpl::true_)
        {
            return iter;
        };

        template <typename First,typename Last,typename Pred,typename Iterator>
        typename static_find_if<First,Last,Pred>::type
        call(static_find_if<First,Last,Pred> const& obj,Iterator const& iter, mpl::false_)
        {
            return call(obj,fusion::next(iter));
        };

        template <typename First,typename Last,typename Pred,typename Iterator>
        typename main_find_if<First,Last,typename mpl::lambda<Pred>::type>::type
        call(static_find_if<First,Last,Pred> const& obj,Iterator const& iter)
        {
            typedef meta::equal_to<Iterator, BOOST_DEDUCED_TYPENAME static_find_if<First,Last,Pred>::type> found;
            return call(obj,iter, found());
        };        
    }

}}}

#endif
