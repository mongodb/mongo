# Copyright 2018 Peter Dimov
# Copyright 2018 Andrey Semashev
# Distributed under the Boost Software License, Version 1.0.
# See accompanying file LICENSE_1_0.txt or copy at https://www.boost.org/LICENSE_1_0.txt

cmake_minimum_required(VERSION 3.5...3.20)

project(boost_utility VERSION "${BOOST_SUPERPROJECT_VERSION}" LANGUAGES CXX)

add_library(boost_utility INTERFACE)
add_library(Boost::utility ALIAS boost_utility)

target_include_directories(boost_utility INTERFACE include)

target_link_libraries(boost_utility
    INTERFACE
        Boost::config
        Boost::core
        Boost::io
        Boost::preprocessor
        Boost::static_assert
        Boost::throw_exception
        Boost::type_traits
)
