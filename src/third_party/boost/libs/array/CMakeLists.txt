# Copyright 2018 Mike Dev
# Distributed under the Boost Software License, Version 1.0.
# See accompanying file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt

# NOTE: CMake support for Boost.Array is currently experimental at best
#       and the interface is likely to change in the future

cmake_minimum_required(VERSION 3.5)
project(boost-array LANGUAGES CXX)

add_library(boost_array INTERFACE)
add_library(Boost::array ALIAS boost_array)

target_include_directories(boost_array INTERFACE include)

target_link_libraries(boost_array
    INTERFACE
        Boost::assert
        Boost::config
        Boost::core
        Boost::static_assert
        Boost::throw_exception
)

