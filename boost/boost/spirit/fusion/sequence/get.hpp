/*=============================================================================
    Copyright (c) 1999-2003 Jaakko Järvi
    Copyright (c) 2001-2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_SEQUENCE_GET_HPP)
#define FUSION_SEQUENCE_GET_HPP

#include <boost/spirit/fusion/sequence/at.hpp>
#include <boost/spirit/fusion/sequence/tuple_element.hpp>
#include <boost/spirit/fusion/sequence/detail/sequence_base.hpp>
#include <boost/type_traits/add_reference.hpp>

namespace boost { namespace fusion
{
    namespace detail
    {
        template<int N>
        struct pair_element;

        template<>
        struct pair_element<0>
        {
            template<typename T1, typename T2>
            static BOOST_DEDUCED_TYPENAME add_reference<T1>::type 
            get(std::pair<T1, T2>& pr)
            {
                return pr.first;
            }

            template<typename T1, typename T2>
            static BOOST_DEDUCED_TYPENAME add_reference<const T1>::type
            get(const std::pair<T1, T2>& pr)
            {
                return pr.first;
            }
        };

        template<>
        struct pair_element<1>
        {
            template<typename T1, typename T2>
            static BOOST_DEDUCED_TYPENAME add_reference<T2>::type
            get(std::pair<T1, T2>& pr)
            {
                return pr.second;
            }

            template<typename T1, typename T2>
            static BOOST_DEDUCED_TYPENAME add_reference<const T2>::type
            get(const std::pair<T1, T2>& pr)
            {
                return pr.second;
            }
        };
    }

    ///////////////////////////////////////////////////////////////////////////
    //
    //  get function
    //
    //      Given a constant integer N and a sequence, returns a reference to
    //      the Nth element of the sequence. (N is a zero based index). Usage:
    //
    //          get<N>(seq)
    //
    //  This function is provided here for compatibility with the tuples TR1
    //  specification. This function forwards to at<N>(seq).
    //
    ///////////////////////////////////////////////////////////////////////////
    template <int N, typename Sequence>
    inline typename meta::at_c<Sequence const, N>::type
    get(sequence_base<Sequence> const& seq FUSION_GET_MSVC_WORKAROUND)
    {
        typedef meta::at_c<Sequence const, N> at_meta;
        return meta::at_impl<BOOST_DEDUCED_TYPENAME at_meta::seq::tag>::
            template apply<BOOST_DEDUCED_TYPENAME at_meta::seq const, N>::call(
                at_meta::seq_converter::convert_const(seq.cast()));

//        return at<N>(seq.cast());
    }

    template <int N, typename Sequence>
    inline typename meta::at_c<Sequence, N>::type
    get(sequence_base<Sequence>& seq FUSION_GET_MSVC_WORKAROUND)
    {
        typedef meta::at_c<Sequence, N> at_meta;
        return meta::at_impl<BOOST_DEDUCED_TYPENAME at_meta::seq::tag>::
            template apply<BOOST_DEDUCED_TYPENAME at_meta::seq, N>::call(
                at_meta::seq_converter::convert(seq.cast()));
//        return at<N>(seq.cast());
    }

    template<int N, typename T1, typename T2>
    inline BOOST_DEDUCED_TYPENAME boost::add_reference<
        BOOST_DEDUCED_TYPENAME tuple_element<N, std::pair<T1, T2> >::type>::type
    get(std::pair<T1, T2>& pr)
    {
        return detail::pair_element<N>::get(pr);
    }

    template<int N, typename T1, typename T2>
    inline BOOST_DEDUCED_TYPENAME boost::add_reference<
        const BOOST_DEDUCED_TYPENAME tuple_element<N, std::pair<T1, T2> >::type>::type
    get(const std::pair<T1, T2>& pr)
    {
        return detail::pair_element<N>::get(pr);
    }
}}

#endif

