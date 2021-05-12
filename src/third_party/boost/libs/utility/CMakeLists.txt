# Copyright 2018 Peter Dimov
# Copyright 2018 Andrey Semashev
# Distributed under the Boost Software License, Version 1.0.
# See accompanying file LICENSE_1_0.txt or copy at https://www.boost.org/LICENSE_1_0.txt

# Partial (add_subdirectory only) and experimental CMake support
# Subject to change; please do not rely on the contents of this file yet.

cmake_minimum_required(VERSION 3.5)

project(BoostUtility LANGUAGES CXX)

add_library(boost_utility INTERFACE)
add_library(Boost::utility ALIAS boost_utility)

target_include_directories(boost_utility INTERFACE include)

target_link_libraries(boost_utility
    INTERFACE
        Boost::config
        Boost::container_hash
        Boost::core
        Boost::io
        Boost::preprocessor
        Boost::static_assert
        Boost::throw_exception
        Boost::type_traits
)
