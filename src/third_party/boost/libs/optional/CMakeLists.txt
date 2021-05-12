# Copyright 2019 Mike Dev
# Distributed under the Boost Software License, Version 1.0.
# See accompanying file LICENSE_1_0.txt or copy at https://www.boost.org/LICENSE_1_0.txt
#
# NOTE: CMake support for Boost.Optional is currently experimental at best
#       and the interface is likely to change in the future

cmake_minimum_required( VERSION 3.5 )
project( BoostOptional )

add_library( boost_optional INTERFACE )
add_library( Boost::optional ALIAS boost_optional )

target_include_directories( boost_optional INTERFACE include )

target_link_libraries( boost_optional
    INTERFACE
        Boost::assert
        Boost::config
        Boost::core
        Boost::detail
        Boost::move
        Boost::predef
        Boost::static_assert
        Boost::throw_exception
        Boost::type_traits
        Boost::utility
)
