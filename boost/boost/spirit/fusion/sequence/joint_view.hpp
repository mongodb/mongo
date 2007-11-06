/*=============================================================================
    Copyright (c) 2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_SEQUENCE_JOINT_VIEW_HPP)
#define FUSION_SEQUENCE_JOINT_VIEW_HPP

#include <boost/spirit/fusion/detail/access.hpp>
#include <boost/spirit/fusion/sequence/begin.hpp>
#include <boost/spirit/fusion/sequence/end.hpp>
#include <boost/spirit/fusion/iterator/joint_view_iterator.hpp>
#include <boost/spirit/fusion/sequence/detail/joint_view_begin_end_traits.hpp>
#include <boost/spirit/fusion/sequence/detail/sequence_base.hpp>
#include <boost/spirit/fusion/sequence/as_fusion_sequence.hpp>

#include <boost/mpl/if.hpp>

namespace boost { namespace fusion
{
    struct joint_view_tag;

    template<typename View1, typename View2, bool copy1, bool copy2>
    struct joint_view;

    template <typename View1, typename View2, bool copy1 = false, bool copy2 = false>
    struct joint_view : sequence_base<joint_view<View1, View2, copy1, copy2> >
    {
        typedef as_fusion_sequence<View1> view1_converter;
        typedef typename view1_converter::type view1;
        typedef as_fusion_sequence<View2> view2_converter;
        typedef typename view2_converter::type view2;

        typedef joint_view_tag tag;
        typedef typename meta::begin<view1>::type first_type;
        typedef typename meta::end<view1>::type last_type;
        typedef typename meta::begin<view2>::type concat_type;
        typedef typename meta::end<view2>::type concat_last_type;

        joint_view(View1& view1, View2& view2);

        first_type first() const { return boost::fusion::begin(view1_); }
        concat_type concat() const { return boost::fusion::begin(view2_); }
        concat_last_type concat_last() const { return boost::fusion::end(view2_); }

    private:
        typename boost::mpl::if_c<copy1, View1, View1&>::type view1_;
        typename boost::mpl::if_c<copy2, View2, View2&>::type view2_;
    };

    template <typename View1, typename View2, bool copy1, bool copy2>
    joint_view<View1,View2,copy1,copy2>::joint_view(View1& view1, View2& view2)
        : view1_(view1), view2_(view2)
    {}

}}

#endif


