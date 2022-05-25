# Copyright 2018 Glen Joseph Fernandes
# (glenjofe@gmail.com)
#
# Distributed under the Boost Software License, Version 1.0.
# (http://www.boost.org/LICENSE_1_0.txt)

cmake_minimum_required(VERSION 3.5...3.20)

project(boost_align VERSION "${BOOST_SUPERPROJECT_VERSION}" LANGUAGES CXX)

add_library(boost_align INTERFACE)

add_library(Boost::align ALIAS boost_align)

target_include_directories(boost_align INTERFACE include)

target_link_libraries(boost_align INTERFACE
    Boost::assert
    Boost::config
    Boost::core
    Boost::static_assert
)
