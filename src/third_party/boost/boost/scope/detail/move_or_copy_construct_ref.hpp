/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * https://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2022 Andrey Semashev
 */
/*!
 * \file scope/detail/move_or_copy_construct_ref.hpp
 *
 * This header contains definition of \c move_or_copy_construct_ref type trait.
 */

#ifndef BOOST_SCOPE_DETAIL_MOVE_OR_COPY_CONSTRUCT_REF_HPP_INCLUDED_
#define BOOST_SCOPE_DETAIL_MOVE_OR_COPY_CONSTRUCT_REF_HPP_INCLUDED_

#include <type_traits>
#include <boost/scope/detail/config.hpp>
#include <boost/scope/detail/header.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

namespace boost {
namespace scope {
namespace detail {

//! The type trait produces an rvalue reference to \a From if \a To has a non-throwing constructor from a \a From rvalue and an lvalue reference otherwise.
template< typename From, typename To = From >
struct move_or_copy_construct_ref
{
    using type = typename std::conditional<
        std::is_nothrow_constructible< To, From >::value,
        From&&,
        From const&
    >::type;
};

template< typename From, typename To >
struct move_or_copy_construct_ref< From&, To >
{
    using type = From&;
};

} // namespace detail
} // namespace scope
} // namespace boost

#include <boost/scope/detail/footer.hpp>

#endif // BOOST_SCOPE_DETAIL_MOVE_OR_COPY_CONSTRUCT_REF_HPP_INCLUDED_
