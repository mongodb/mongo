/*=============================================================================
    Copyright (c) 2003 Joel de Guzman

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_DETAIL_ACCESS_HPP)
#define FUSION_DETAIL_ACCESS_HPP

#include <boost/spirit/fusion/detail/config.hpp>
#include <boost/mpl/eval_if.hpp>
#include <boost/mpl/identity.hpp>
#include <boost/type_traits/add_const.hpp>
#include <boost/type_traits/add_reference.hpp>
#include <boost/type_traits/is_reference.hpp>
#include <boost/type_traits/remove_cv.hpp>

namespace boost { namespace fusion { namespace detail
{
    template <typename T>
    struct ref_result
    {
        typedef typename add_reference<FUSION_GET_TYPE(T)>::type type;
    };

    template <typename T>
    struct cref_result
    {
        typedef typename add_reference<
            typename add_const<FUSION_GET_TYPE(T)>::type
        >::type type;
    };

    template <typename T>
    struct non_ref_parameter
    {
        typedef typename boost::remove_cv<T>::type const& type;
    };

    template <typename T>
    struct call_param
    {
        typedef
            typename mpl::eval_if<
                is_reference<T>
              , mpl::identity<T>
              , non_ref_parameter<T>
            >::type
        type;
    };
}}}

#endif

