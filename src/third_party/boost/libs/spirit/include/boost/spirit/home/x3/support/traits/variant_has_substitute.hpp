/*=============================================================================
    Copyright (c) 2001-2014 Joel de Guzman
    http://spirit.sourceforge.net/

    Distributed under the Boost Software License, Version 1.0. (See accompanying
    file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#if !defined(BOOST_SPIRIT_X3_VARIANT_HAS_SUBSTITUTE_APR_18_2014_925AM)
#define BOOST_SPIRIT_X3_VARIANT_HAS_SUBSTITUTE_APR_18_2014_925AM

#include <boost/spirit/home/x3/support/traits/is_substitute.hpp>
#include <boost/mpl/find.hpp>

namespace boost { namespace spirit { namespace x3 { namespace traits
{
    template <typename Variant, typename T>
    struct variant_has_substitute_impl
    {
        // Find a type from the Variant that can be a substitute for T.
        // return true_ if one is found, else false_

        typedef Variant variant_type;
        typedef typename variant_type::types types;
        typedef typename mpl::end<types>::type end;

        typedef typename mpl::find<types, T>::type iter_1;

        typedef typename
            mpl::eval_if<
                is_same<iter_1, end>,
                mpl::find_if<types, traits::is_substitute<T, mpl::_1>>,
                mpl::identity<iter_1>
            >::type
        iter;

        typedef mpl::not_<is_same<iter, end>> type;
    };

    template <typename Variant, typename T>
    struct variant_has_substitute
        : variant_has_substitute_impl<Variant, T>::type {};

    template <typename T>
    struct variant_has_substitute<unused_type, T> : mpl::true_ {};

    template <typename T>
    struct variant_has_substitute<unused_type const, T> : mpl::true_ {};

}}}}

#endif
