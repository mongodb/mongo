//  filesystem/string_file.hpp  --------------------------------------------------------//

//  Copyright Beman Dawes 2015

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  Library home page: http://www.boost.org/libs/filesystem

#ifndef BOOST_FILESYSTEM_STRING_FILE_HPP
#define BOOST_FILESYSTEM_STRING_FILE_HPP

#include <boost/filesystem/config.hpp>

#if !defined(BOOST_FILESYSTEM_NO_DEPRECATED)

#if !defined(BOOST_FILESYSTEM_DEPRECATED)
#include <boost/config/header_deprecated.hpp>
BOOST_HEADER_DEPRECATED("your own implementation")
#endif

#include <cstddef>
#include <limits>
#include <string>
#include <ios>
#include <stdexcept>
#include <boost/cstdint.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>

#include <boost/filesystem/detail/header.hpp> // must be the last #include

namespace boost {
namespace filesystem {

inline void save_string_file(path const& p, std::string const& str)
{
    filesystem::ofstream file;
    file.exceptions(std::ios_base::failbit | std::ios_base::badbit);
    file.open(p, std::ios_base::binary);
    const std::size_t sz = str.size();
    if (BOOST_UNLIKELY(sz > static_cast< boost::uintmax_t >((std::numeric_limits< std::streamsize >::max)())))
        BOOST_FILESYSTEM_THROW(std::length_error("String size exceeds max write size"));
    file.write(str.c_str(), static_cast< std::streamsize >(sz));
}

inline void load_string_file(path const& p, std::string& str)
{
    filesystem::ifstream file;
    file.exceptions(std::ios_base::failbit | std::ios_base::badbit);
    file.open(p, std::ios_base::binary);
    const boost::uintmax_t sz = filesystem::file_size(p);
    if (BOOST_UNLIKELY(sz > static_cast< boost::uintmax_t >((std::numeric_limits< std::streamsize >::max)())))
        BOOST_FILESYSTEM_THROW(std::length_error("File size exceeds max read size"));
    str.resize(static_cast< std::size_t >(sz), '\0');
    if (sz > 0u)
        file.read(&str[0], static_cast< std::streamsize >(sz));
}

} // namespace filesystem
} // namespace boost

#include <boost/filesystem/detail/footer.hpp>

#endif // !defined(BOOST_FILESYSTEM_NO_DEPRECATED)

#endif // BOOST_FILESYSTEM_STRING_FILE_HPP
