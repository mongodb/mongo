# Copyright 2022 Andrey Semashev
#
# Distributed under the Boost Software License, Version 1.0.
# See accompanying file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt
#
# After including this module, BOOST_LIBRARY_INCLUDES variable is set to the list of include
# directories for all Boost libraries. If the monolithic include directory is found, it is
# used instead.

if (NOT CMAKE_VERSION VERSION_LESS 3.10)
    include_guard()
endif()

# Generates a list of include paths for all Boost libraries in \a result variable. Uses unified Boost include tree, if available.
function(generate_boost_include_paths result)
    if (IS_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/../../../boost" AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../../boost/version.hpp")
        get_filename_component(include_dir "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
        set(${result} "${include_dir}" PARENT_SCOPE)
        return()
    endif()
    file(GLOB path_list LIST_DIRECTORIES True "${CMAKE_CURRENT_LIST_DIR}/../../../libs/*")
    foreach(path IN LISTS path_list)
        if (IS_DIRECTORY "${path}/include")
            get_filename_component(include_dir "${path}/include" ABSOLUTE)
            list(APPEND include_list "${include_dir}")
        endif()
    endforeach()
    set(${result} ${include_list} PARENT_SCOPE)
endfunction()

if (NOT DEFINED BOOST_LIBRARY_INCLUDES)
    generate_boost_include_paths(__BOOST_LIBRARY_INCLUDES)
    # Save the paths in a global property to avoid scanning the filesystem if this module is used in multiple libraries
    set(BOOST_LIBRARY_INCLUDES ${__BOOST_LIBRARY_INCLUDES} CACHE INTERNAL "List of all Boost library include paths")
    unset(__BOOST_LIBRARY_INCLUDES)
    # message(STATUS "Boost library includes: ${BOOST_LIBRARY_INCLUDES}")
endif()
