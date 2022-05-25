# Copyright 2018 Peter Dimov
# Copyright 2018 Andrey Semashev
# Distributed under the Boost Software License, Version 1.0.
# See accompanying file LICENSE_1_0.txt or copy at https://www.boost.org/LICENSE_1_0.txt

cmake_minimum_required(VERSION 3.5...3.20)

project(boost_integer VERSION "${BOOST_SUPERPROJECT_VERSION}" LANGUAGES CXX)

add_library(boost_integer INTERFACE)
add_library(Boost::integer ALIAS boost_integer)

target_include_directories(boost_integer INTERFACE include)

target_link_libraries(boost_integer
    INTERFACE
        Boost::assert
        Boost::config
        Boost::core
        Boost::static_assert
        Boost::throw_exception
)
