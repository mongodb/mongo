/*=============================================================================
    Copyright (c) 2022 Denis Mikhailov
    Distributed under the Boost Software License, Version 1.0. (See accompanying
    file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/

#if !defined(BOOST_FUSION_IDENTITY_VIEW_HPP_INCLUDED)
#define BOOST_FUSION_IDENTITY_VIEW_HPP_INCLUDED

#include <boost/fusion/support/config.hpp>
#include <boost/fusion/view/transform_view.hpp>
#include <boost/functional/identity.hpp>
#include <boost/utility/result_of.hpp>

namespace boost { namespace fusion {
    namespace detail {
        struct identity : boost::identity
        {
        };
    }
}}

namespace boost {
    template<typename T>
    struct result_of<fusion::detail::identity(T)>
    {
        typedef T type;
    };
}

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4512) // assignment operator could not be generated.
#endif
namespace boost { namespace fusion {
    template<typename Sequence> struct identity_view 
        : transform_view<Sequence, detail::identity>
    {
        typedef transform_view<Sequence, detail::identity> base_type;

        BOOST_CONSTEXPR BOOST_FUSION_GPU_ENABLED
        identity_view(Sequence& in_seq)
            : base_type(in_seq, detail::identity()) {}
    };
}}
#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif 
