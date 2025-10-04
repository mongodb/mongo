/* Copyright 2003-2023 Joaquin M Lopez Munoz.
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org/libs/multi_index for library home page.
 */

#ifndef BOOST_MULTI_INDEX_DETAIL_BAD_ARCHIVE_EXCEPTION_HPP
#define BOOST_MULTI_INDEX_DETAIL_BAD_ARCHIVE_EXCEPTION_HPP

#if defined(_MSC_VER)
#pragma once
#endif

#include <boost/config.hpp> /* keep it first to prevent nasty warns in MSVC */
#include <stdexcept>

namespace boost{

namespace multi_index{

namespace detail{

struct bad_archive_exception:std::runtime_error
{
  bad_archive_exception():std::runtime_error("Invalid or corrupted archive"){}
};

} /* namespace multi_index::detail */

} /* namespace multi_index */

} /* namespace boost */

#endif
