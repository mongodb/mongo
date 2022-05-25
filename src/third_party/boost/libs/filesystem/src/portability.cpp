//  portability.cpp  -------------------------------------------------------------------//

//  Copyright 2002-2005 Beman Dawes
//  Use, modification, and distribution is subject to the Boost Software
//  License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy
//  at http://www.boost.org/LICENSE_1_0.txt)

//  See library home page at http://www.boost.org/libs/filesystem

//--------------------------------------------------------------------------------------//

#include "platform_config.hpp"

#include <boost/filesystem/config.hpp>
#include <boost/filesystem/path.hpp>

#include <cstring> // SGI MIPSpro compilers need this
#include <string>

namespace boost {
namespace filesystem {

namespace {

BOOST_CONSTEXPR_OR_CONST char windows_invalid_chars[] =
    "\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0A\x0B\x0C\x0D\x0E\x0F"
    "\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1A\x1B\x1C\x1D\x1E\x1F"
    "<>:\"/\\|";

BOOST_CONSTEXPR_OR_CONST char posix_valid_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-";

} // unnamed namespace

//  name_check functions  ----------------------------------------------//

#ifdef BOOST_WINDOWS
BOOST_FILESYSTEM_DECL bool native(std::string const& name)
{
    return windows_name(name);
}
#else
BOOST_FILESYSTEM_DECL bool native(std::string const& name)
{
    return !name.empty() && name[0] != ' ' && name.find('/') == std::string::npos;
}
#endif

BOOST_FILESYSTEM_DECL bool portable_posix_name(std::string const& name)
{
    return !name.empty() && name.find_first_not_of(posix_valid_chars, 0, sizeof(posix_valid_chars) - 1) == std::string::npos;
}

BOOST_FILESYSTEM_DECL bool windows_name(std::string const& name)
{
    // note that the terminating '\0' is part of the string - thus the size below
    // is sizeof(windows_invalid_chars) rather than sizeof(windows_invalid_chars)-1.
    return !name.empty() && name[0] != ' ' && name.find_first_of(windows_invalid_chars, 0, sizeof(windows_invalid_chars)) == std::string::npos
        && *(name.end() - 1) != ' ' && (*(name.end() - 1) != '.' || name.size() == 1 || name == "..");
}

BOOST_FILESYSTEM_DECL bool portable_name(std::string const& name)
{
    return !name.empty() && (name == "." || name == ".." || (windows_name(name) && portable_posix_name(name) && name[0] != '.' && name[0] != '-'));
}

BOOST_FILESYSTEM_DECL bool portable_directory_name(std::string const& name)
{
    return name == "." || name == ".." || (portable_name(name) && name.find('.') == std::string::npos);
}

BOOST_FILESYSTEM_DECL bool portable_file_name(std::string const& name)
{
    std::string::size_type pos;
    return portable_name(name) && name != "." && name != ".." && ((pos = name.find('.')) == std::string::npos || (name.find('.', pos + 1) == std::string::npos && (pos + 5) > name.size()));
}

} // namespace filesystem
} // namespace boost
