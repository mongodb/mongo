//
// Copyright (c) Chris Glover, 2016.
//
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_TYPE_INDEX_RUNTIME_CAST_DETAIL_RUNTIME_CAST_IMPL_HPP
#define BOOST_TYPE_INDEX_RUNTIME_CAST_DETAIL_RUNTIME_CAST_IMPL_HPP

/// \file runtime_cast_impl.hpp
/// \brief Contains the overload of boost::typeindex::runtime_cast for
/// pointer types.
///
/// boost::typeindex::runtime_cast can be used to emulate dynamic_cast
/// functionality on platorms that don't provide it or should the user
/// desire opt in functionality instead of enabling it system wide.

#include <boost/type_index.hpp>

#include <type_traits>

#ifdef BOOST_HAS_PRAGMA_ONCE
# pragma once
#endif

namespace boost { namespace typeindex {

namespace detail {

template<typename T, typename U>
T* runtime_cast_impl(U* u, std::integral_constant<bool, true>) noexcept {
    return u;
}

template<typename T, typename U>
T const* runtime_cast_impl(U const* u, std::integral_constant<bool, true>) noexcept {
    return u;
}

template<typename T, typename U>
T* runtime_cast_impl(U* u, std::integral_constant<bool, false>) noexcept {
    return const_cast<T*>(static_cast<T const*>(
        u->boost_type_index_find_instance_(boost::typeindex::type_id<T>())
    ));
}

template<typename T, typename U>
T const* runtime_cast_impl(U const* u, std::integral_constant<bool, false>) noexcept {
    return static_cast<T const*>(u->boost_type_index_find_instance_(boost::typeindex::type_id<T>()));
}

} // namespace detail

}} // namespace boost::typeindex

#endif // BOOST_TYPE_INDEX_RUNTIME_CAST_DETAIL_RUNTIME_CAST_IMPL_HPP
