/*=============================================================================
    Copyright (c) 1999-2003 Jaakko Järvi
    Copyright (c) 2001-2003 Joel de Guzman

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_SEQUENCE_DETAIL_AS_TUPLE_ELEMENT_HPP)
#define FUSION_SEQUENCE_DETAIL_AS_TUPLE_ELEMENT_HPP

#include <boost/ref.hpp>

#if defined BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
# include <boost/mpl/eval_if.hpp>
# include <boost/mpl/identity.hpp>
# include <boost/type_traits/is_array.hpp>
# include <boost/type_traits/is_convertible.hpp>
#endif

namespace boost { namespace fusion { namespace detail
{
#if !defined BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION

    template <typename T>
    struct as_tuple_element
    {
        typedef T type;
    };

    template <typename T>
    struct as_tuple_element<reference_wrapper<T> >
    {
        typedef T& type;
    };

#if !BOOST_WORKAROUND(__MWERKS__, <= 0x2407)

    template <typename T>
    struct as_tuple_element<reference_wrapper<T> const>
    {
        typedef T& type;
    };

#endif

    template <typename T, int N>
    struct as_tuple_element<T[N]>
    {
        typedef const T(&type)[N];
    };

    template <typename T, int N>
    struct as_tuple_element<volatile T[N]>
    {
        typedef const volatile T(&type)[N];
    };

    template <typename T, int N>
    struct as_tuple_element<const volatile T[N]>
    {
        typedef const volatile T(&type)[N];
    };

#else

    //  The Non-PTS version cannot accept arrays since there is no way to
    //  get the element type of an array T[N]. However, we shall provide
    //  the most common case where the array is a char[N] or wchar_t[N].
    //  Doing so will allow literal string argument types.

    template <typename T>
    struct maybe_string
    {
        typedef typename
            mpl::eval_if<
                is_array<T>
              , mpl::eval_if<
                    is_convertible<T, char const*>
                  , mpl::identity<char const*>
                  , mpl::eval_if<
                        is_convertible<T, wchar_t const*>
                      , mpl::identity<wchar_t const*>
                      , mpl::identity<T>
                    >
                >
              , mpl::identity<T>
            >::type
        type;
    };

    template <typename T>
    struct as_tuple_element
    {
        typedef typename
            mpl::eval_if<
                is_reference_wrapper<T>
              , add_reference<typename unwrap_reference<T>::type>
              , maybe_string<T>
            >::type
        type;
    };

#endif

}}}

#endif
