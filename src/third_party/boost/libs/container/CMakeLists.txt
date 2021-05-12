# Copyright 2019 Mike Dev
# Distributed under the Boost Software License, Version 1.0.
# See accompanying file LICENSE_1_0.txt or copy at https://www.boost.org/LICENSE_1_0.txt
#
# NOTE: CMake support for Boost.Container is currently experimental at best
#       and the interface is likely to change in the future

cmake_minimum_required( VERSION 3.5 )
project( BoostContainer LANGUAGES C CXX )

file( GLOB boost_container_cpp_files src/*.cpp )

add_library(boost_container
    ${boost_container_cpp_files}
    src/alloc_lib.c
)

# This is the public target name, other libraries should link to
add_library( Boost::container ALIAS boost_container )

target_include_directories( boost_container PUBLIC include )

# NOTE:
# We deactivate autolinking, because cmake based builds don't need it and
# we don't implement name mangling for the library file anyway.
# Ususally the parent CMakeLists.txt file should already have globally defined BOOST_ALL_NO_LIB
target_compile_definitions( boost_container PUBLIC BOOST_CONTAINER_NO_LIB )

target_link_libraries( boost_container
    PUBLIC
        Boost::assert
        Boost::config
        Boost::container_hash
        Boost::core
        Boost::intrusive
        Boost::move
        Boost::static_assert
        Boost::type_traits
)
