/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * https://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2022 Andrey Semashev
 */
/*!
 * \file scope/unique_resource_fwd.hpp
 *
 * This header contains forward declaration of \c unique_resource template.
 */

#ifndef BOOST_SCOPE_UNIQUE_RESOURCE_FWD_HPP_INCLUDED_
#define BOOST_SCOPE_UNIQUE_RESOURCE_FWD_HPP_INCLUDED_

#include <type_traits>
#include <boost/scope/detail/config.hpp>
#include <boost/scope/detail/move_or_copy_construct_ref.hpp>
#include <boost/scope/detail/type_traits/conjunction.hpp>
#include <boost/scope/detail/header.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

namespace boost {
namespace scope {

template< typename Resource, typename Deleter, typename Traits = void >
class unique_resource;

template< typename Resource, typename Deleter, typename Invalid = typename std::decay< Resource >::type >
unique_resource< typename std::decay< Resource >::type, typename std::decay< Deleter >::type >
make_unique_resource_checked(Resource&& res, Invalid const& invalid, Deleter&& del)
    noexcept(BOOST_SCOPE_DETAIL_DOC_HIDDEN(detail::conjunction<
        std::is_nothrow_constructible< typename std::decay< Resource >::type, typename detail::move_or_copy_construct_ref< Resource, typename std::decay< Resource >::type >::type >,
        std::is_nothrow_constructible< typename std::decay< Deleter >::type, typename detail::move_or_copy_construct_ref< Deleter, typename std::decay< Deleter >::type >::type >
    >::value));

} // namespace scope
} // namespace boost

#include <boost/scope/detail/footer.hpp>

#endif // BOOST_SCOPE_UNIQUE_RESOURCE_FWD_HPP_INCLUDED_
