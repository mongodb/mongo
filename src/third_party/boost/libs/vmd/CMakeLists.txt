# Copyright 2018 Mike Dev
# Distributed under the Boost Software License, Version 1.0.
# See accompanying file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt

cmake_minimum_required(VERSION 3.5...3.20)
project(boost_vmd VERSION "${BOOST_SUPERPROJECT_VERSION}" LANGUAGES NONE)

add_library(boost_vmd INTERFACE)
add_library(Boost::vmd ALIAS boost_vmd)

target_include_directories(boost_vmd INTERFACE include)

target_link_libraries(boost_vmd
    INTERFACE
        Boost::preprocessor
)
