
// (C) Copyright Eric Friedman 2002-2003.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org for most recent version including documentation.

#ifndef BOOST_VARIANT_DETAIL_HAS_NOTHROW_MOVE_HPP_INCLUDED
#define BOOST_VARIANT_DETAIL_HAS_NOTHROW_MOVE_HPP_INCLUDED

#include "boost/config.hpp" // for STATIC_CONSTANT
#include "boost/variant/detail/has_trivial_move.hpp"
#include "boost/type_traits/has_nothrow_copy.hpp"
#include "boost/type_traits/has_nothrow_assign.hpp"

#include "boost/mpl/and.hpp"
#include "boost/mpl/or.hpp"

// should be the last #include
#include "boost/variant/detail/bool_trait_def.hpp"

namespace boost {
namespace detail { namespace variant {

// TRAIT: has_nothrow_move

template <typename T>
struct has_nothrow_move_impl
{
    BOOST_STATIC_CONSTANT(
        bool, value = (
            ::boost::mpl::or_<
                has_trivial_move<T>
              , ::boost::mpl::and_<
                  has_nothrow_copy<T>
                , has_nothrow_assign<T>
                >
            >::type::value
            )
        );
};

BOOST_VARIANT_TT_AUX_BOOL_TRAIT_DEF1(
      has_nothrow_move
    , T
    , (::boost::detail::variant::has_nothrow_move_impl<T>::value)
    )


// TRAIT: has_nothrow_move_constructor

template <typename T>
struct has_nothrow_move_constructor_impl
{
    BOOST_STATIC_CONSTANT(
        bool, value = (
            ::boost::mpl::or_<
              has_nothrow_move<T>
            , has_trivial_move_constructor<T>
            , has_nothrow_copy<T>
            >::type::value
            )
        );
};

BOOST_VARIANT_TT_AUX_BOOL_TRAIT_DEF1(
      has_nothrow_move_constructor
    , T
    , (::boost::detail::variant::has_nothrow_move_constructor_impl<T>::value)
    )


// TRAIT: has_nothrow_move_assign

template <typename T>
struct has_nothrow_move_assign_impl
{
    BOOST_STATIC_CONSTANT(
        bool, value = (
            ::boost::mpl::or_<
              has_nothrow_move<T>
            , has_trivial_move_assign<T>
            , has_nothrow_assign<T>
            >::type::value
            )
        );
};

BOOST_VARIANT_TT_AUX_BOOL_TRAIT_DEF1(
      has_nothrow_move_assign
    , T
    , (::boost::detail::variant::has_nothrow_move_assign_impl<T>::value)
    )

}} // namespace detail::variant

BOOST_VARIANT_TT_AUX_TRAIT_SUFFIX(1,::boost::detail::variant::has_nothrow_move)
BOOST_VARIANT_TT_AUX_TRAIT_SUFFIX(1,::boost::detail::variant::has_nothrow_move_constructor)
BOOST_VARIANT_TT_AUX_TRAIT_SUFFIX(1,::boost::detail::variant::has_nothrow_move_assign)

} // namespace boost

#include "boost/variant/detail/bool_trait_undef.hpp"

#endif // BOOST_VARIANT_DETAIL_HAS_NOTHROW_MOVE_HPP_INCLUDED
