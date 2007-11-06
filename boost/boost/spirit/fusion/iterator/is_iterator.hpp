/*=============================================================================
    Copyright (c) 2003 Joel de Guzman

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ITERATOR_IS_ITERATOR_HPP)
#define FUSION_ITERATOR_IS_ITERATOR_HPP

#include <boost/type_traits/is_base_and_derived.hpp>

namespace boost { namespace fusion
{
    ///////////////////////////////////////////////////////////////////////////
    //
    //  is_iterator metafunction
    //
    //      Given a type T, returns a value true or false if T is a
    //      fusion iterator or not. Usage:
    //
    //          is_iterator<T>::value
    //
    ///////////////////////////////////////////////////////////////////////////
    struct iterator_root;

    template <typename T>
    struct is_iterator : is_base_and_derived<iterator_root, T> {};

}}

#endif
