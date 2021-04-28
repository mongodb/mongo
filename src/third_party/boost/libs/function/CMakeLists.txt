# Copyright 2019 Peter Dimov
# Distributed under the Boost Software License, Version 1.0.
# See accompanying file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt

# Partial (add_subdirectory only) and experimental CMake support
# Subject to change; please do not rely on the contents of this file yet

cmake_minimum_required(VERSION 3.5)
project(BoostFunction LANGUAGES CXX)

add_library(boost_function INTERFACE)
add_library(Boost::function ALIAS boost_function)

target_include_directories(boost_function INTERFACE include)

target_link_libraries(boost_function
    INTERFACE
        Boost::assert
        Boost::bind
        Boost::config
        Boost::core
        Boost::integer
        Boost::preprocessor
        Boost::throw_exception
        Boost::type_index
        Boost::type_traits
        Boost::typeof
)
