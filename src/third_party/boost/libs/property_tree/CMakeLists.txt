#
# Copyright (c) 2019 Vinnie Falco (vinnie.falco@gmail.com)
# Copyright (c) 2021 Richard Hodges (hodges.r@gmail.com)
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# Official repository: https://github.com/boostorg/property_tree
#

cmake_minimum_required(VERSION 3.5...3.19)

set(BOOST_PROPERTY_TREE_VERSION ${BOOST_SUPERPROJECT_VERSION})

project(boost_property_tree VERSION "${BOOST_PROPERTY_TREE_VERSION}" LANGUAGES CXX)

option(BOOST_PROPERTY_TREE_BUILD_TESTS "Build boost::property_tree tests" ${BUILD_TESTING})
option(BOOST_PROPERTY_TREE_BUILD_EXAMPLES "Build boost::property_tree examples" ${BOOST_PROPERTY_TREE_BUILD_TESTS})

file(GLOB_RECURSE BOOST_PROPERTY_TREE_HEADERS $<$<VERSION_GREATER_EQUAL:${CMAKE_VERSION},3.12>:CONFIGURE_DEPENDS>
    include/boost/*.hpp
    include/boost/*.ipp
    include/boost/*.natvis
)

set(BOOST_PROPERTY_TREE_SOURCES
)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/include/boost PREFIX "" FILES ${BOOST_PROPERTY_TREE_HEADERS})
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/src PREFIX "" FILES ${BOOST_PROPERTY_TREE_SOURCES})

add_library(boost_property_tree INTERFACE)
add_library(Boost::property_tree ALIAS boost_property_tree)

target_include_directories(boost_property_tree INTERFACE include)

if(BOOST_SUPERPROJECT_VERSION)
    #
    # Building as part of Boost superproject tree, with Boost as dependency.
    #
    target_link_libraries(boost_property_tree
      INTERFACE
        Boost::any
        Boost::assert
        Boost::bind
        Boost::config
        Boost::core
        Boost::format
        Boost::iterator
        Boost::mpl
        Boost::multi_index
        Boost::optional
        Boost::range
        Boost::serialization
        Boost::static_assert
        Boost::throw_exception
        Boost::type_traits
    )

elseif(BOOST_PROPERTY_TREE_IN_BOOST_TREE)
    #
    # Building inside Boost tree, out of Boost superproject tree, with Boost as dependency.
    # e.g. on Travis or other CI, or when producing Visual Studio Solution and Projects.
    #
    get_filename_component(BOOST_ROOT ../.. ABSOLUTE)
    set(BOOST_INCLUDEDIR ${BOOST_ROOT})
    set(BOOST_LIBRARYDIR ${BOOST_ROOT}/stage/lib)
    find_package(Boost COMPONENTS serialization REQUIRED)

    target_include_directories(boost_property_tree INTERFACE ${BOOST_ROOT})
    target_link_directories(boost_property_tree INTERFACE ${BOOST_ROOT}/stage/lib)

else()
    #
    # Building out of Boost tree, out of Boost superproject tree, with Boost as dependency.
    # e.g. for packaging or added with add_subdirectory.
    #
    find_package(Boost REQUIRED
            #COMPONENTS container system
            )
    target_link_libraries(boost_property_tree
        INTERFACE
            Boost::boost
    )
endif()

if(BUILD_TESTING)
    include(CTest)
    add_subdirectory(test)
endif()

if(BOOST_PROPERTY_TREE_BUILD_EXAMPLES)
    if (BOOST_SUPERPROJECT_VERSION)
        message(STATUS "[property_tree] superproject build - skipping examples")
    else()
        add_subdirectory(examples)
    endif()
endif()
