# Copyright 2019 Mike Dev
# Distributed under the Boost Software License, Version 1.0.
# See accompanying file LICENSE_1_0.txt or copy at https://www.boost.org/LICENSE_1_0.txt

cmake_minimum_required( VERSION 3.5...3.20 )
project( boost_optional VERSION "${BOOST_SUPERPROJECT_VERSION}" LANGUAGES CXX )

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
