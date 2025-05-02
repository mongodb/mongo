#ifndef BOOST_UUID_NAME_GENERATOR_SHA1_HPP_INCLUDED
#define BOOST_UUID_NAME_GENERATOR_SHA1_HPP_INCLUDED

// Copyright 2010 Andy Tompkins
// Copyright 2017 James E. King III
// Copyright 2024 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/uuid/detail/basic_name_generator.hpp>
#include <boost/uuid/detail/sha1.hpp>

namespace boost {
namespace uuids {

class name_generator_sha1: public detail::basic_name_generator<detail::sha1>
{
public:

    explicit name_generator_sha1( uuid const& namespace_uuid ) noexcept:
        detail::basic_name_generator<detail::sha1>( namespace_uuid )
    {
    }
};

} // uuids
} // boost

#endif // BOOST_UUID_NAME_GENERATOR_SHA1_HPP_INCLUDED
