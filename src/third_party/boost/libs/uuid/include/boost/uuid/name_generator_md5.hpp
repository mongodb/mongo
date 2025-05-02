#ifndef BOOST_UUID_NAME_GENERATOR_MD5_HPP_INCLUDED
#define BOOST_UUID_NAME_GENERATOR_MD5_HPP_INCLUDED

// Copyright 2017 James E. King III
// Copyright 2024 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/uuid/detail/basic_name_generator.hpp>
#include <boost/uuid/detail/md5.hpp>

namespace boost {
namespace uuids {

class name_generator_md5: public detail::basic_name_generator<detail::md5>
{
public:

    explicit name_generator_md5( uuid const& namespace_uuid ) noexcept:
        detail::basic_name_generator<detail::md5>( namespace_uuid )
    {
    }
};

} // uuids
} // boost

#endif // BOOST_UUID_NAME_GENERATOR_MD5_HPP_INCLUDED
