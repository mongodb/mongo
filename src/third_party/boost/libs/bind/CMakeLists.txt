# Copyright 2018, 2019 Peter Dimov
# Distributed under the Boost Software License, Version 1.0.
# See accompanying file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt

cmake_minimum_required(VERSION 3.5...3.16)

project(boost_bind VERSION "${BOOST_SUPERPROJECT_VERSION}" LANGUAGES CXX)

add_library(boost_bind INTERFACE)
add_library(Boost::bind ALIAS boost_bind)

target_include_directories(boost_bind INTERFACE include)

target_link_libraries(boost_bind
  INTERFACE
    Boost::config
    Boost::core
)

if(BUILD_TESTING)

  add_subdirectory(test)

endif()
