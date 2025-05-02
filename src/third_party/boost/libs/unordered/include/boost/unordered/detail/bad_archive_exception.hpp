/* Copyright 2023 Joaquin M Lopez Munoz.
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See https://www.boost.org/libs/unordered for library home page.
 */

#ifndef BOOST_UNORDERED_DETAIL_BAD_ARCHIVE_EXCEPTION_HPP
#define BOOST_UNORDERED_DETAIL_BAD_ARCHIVE_EXCEPTION_HPP

#include <stdexcept>

namespace boost{
namespace unordered{
namespace detail{

struct bad_archive_exception:std::runtime_error
{
  bad_archive_exception():std::runtime_error("Invalid or corrupted archive"){}
};

} /* namespace detail */
} /* namespace unordered */
} /* namespace boost */

#endif
