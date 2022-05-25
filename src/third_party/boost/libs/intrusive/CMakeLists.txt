# Copyright 2019 Mike Dev
# Distributed under the Boost Software License, Version 1.0.
# See accompanying file LICENSE_1_0.txt or copy at https://www.boost.org/LICENSE_1_0.txt

cmake_minimum_required( VERSION 3.5...3.20 )

project( boost_intrusive VERSION "${BOOST_SUPERPROJECT_VERSION}" LANGUAGES CXX )

add_library( boost_intrusive INTERFACE )
add_library( Boost::intrusive ALIAS boost_intrusive )

target_include_directories( boost_intrusive INTERFACE include )

target_link_libraries( boost_intrusive
    INTERFACE
        Boost::assert
        Boost::config
        Boost::container_hash
        Boost::core
        Boost::move
        Boost::static_assert
)
