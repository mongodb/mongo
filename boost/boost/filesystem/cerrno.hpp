//  Boost Filesystem cerrno.hpp header  --------------------------------------//

//  Copyright Beman Dawes 2005.
//  Use, modification, and distribution is subject to the Boost Software
//  License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

//  See library home page at http://www.boost.org/libs/filesystem

#ifndef BOOST_FILESYSTEM_CERRNO_HPP
#define BOOST_FILESYSTEM_CERRNO_HPP

#include <cerrno>

#if defined __BORLANDC__
#define ENOSYS       9997
#endif

#define EBADHANDLE   9998  // bad handle
#define EOTHER       9999  // Other error not translatable
                           // to a POSIX errno value

#endif // include guard
