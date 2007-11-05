/*=============================================================================
    Copyright (c) 2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_SEQUENCE_AS_FUSION_SEQUENCE_HPP)
#define FUSION_SEQUENCE_AS_FUSION_SEQUENCE_HPP

#include <boost/spirit/fusion/sequence/is_sequence.hpp>
#include <boost/spirit/fusion/sequence/type_sequence.hpp>
#include <boost/mpl/is_sequence.hpp>
#include <boost/mpl/if.hpp>
#include <boost/mpl/bool.hpp>
#include <boost/static_assert.hpp>

namespace boost { namespace fusion
{
    //  Test T. If it is a fusion sequence, return a reference to it.
    //  else, assume it is an mpl sequence. Fail if it is not.

    namespace fusion_sequence_detail {
        template<typename T>
        static T const& convert_const(T const& x, mpl::true_) {
            return x;
        }
        template<typename T>
        static type_sequence<T const> convert_const(T const& x, mpl::false_) {
            BOOST_STATIC_ASSERT(mpl::is_sequence<T>::value);
            return type_sequence<T const>();
        }
        template<typename T>
        static T& convert(T& x, mpl::true_)
        {
            return x;
        }

        template<typename T>
        static type_sequence<T> convert(T& x, mpl::false_)
        {
            BOOST_STATIC_ASSERT(mpl::is_sequence<T>::value);
            return type_sequence<T>();
        }
    }

    template <typename T>
    struct as_fusion_sequence {
        typedef typename
            mpl::if_<
                fusion::is_sequence<T>
              , T
              , type_sequence<T>
            >::type
        type;

        static typename
        mpl::if_<
            fusion::is_sequence<T>
          , T const&
          , type_sequence<T const>
        >::type
        convert_const(T const& x);
        
        static typename
        mpl::if_<
            fusion::is_sequence<T>
          , T &
          , type_sequence<T>
        >::type
        convert(T& x);
    };

    template<typename T>
    typename
    mpl::if_<
        fusion::is_sequence<T>
      , T const&
      , type_sequence<T const>
    >::type
    as_fusion_sequence<T>::convert_const(T const& x) {
        return fusion_sequence_detail::convert_const(x,fusion::is_sequence<T>());
    }

    template<typename T>
    typename
    mpl::if_<
        fusion::is_sequence<T>
      , T&
      , type_sequence<T>
    >::type
    as_fusion_sequence<T>::convert(T& x) {
        return fusion_sequence_detail::convert(x,fusion::is_sequence<T>());
    }
}}


#endif
