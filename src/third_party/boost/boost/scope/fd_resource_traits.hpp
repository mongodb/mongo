/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * https://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2023 Andrey Semashev
 */
/*!
 * \file scope/fd_resource_traits.hpp
 *
 * This header contains definition of \c unique_resource traits
 * for compatibility with POSIX-like file descriptors.
 */

#ifndef BOOST_SCOPE_FD_RESOURCE_TRAITS_HPP_INCLUDED_
#define BOOST_SCOPE_FD_RESOURCE_TRAITS_HPP_INCLUDED_

#include <boost/scope/detail/config.hpp>
#include <boost/scope/detail/header.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

namespace boost {
namespace scope {

//! POSIX-like file descriptor resource traits
struct fd_resource_traits
{
    //! Creates a default fd value
    static int make_default() noexcept
    {
        return -1;
    }

    //! Tests if the fd is allocated (valid)
    static bool is_allocated(int fd) noexcept
    {
        return fd >= 0;
    }
};

} // namespace scope
} // namespace boost

#include <boost/scope/detail/footer.hpp>

#endif // BOOST_SCOPE_FD_RESOURCE_TRAITS_HPP_INCLUDED_
