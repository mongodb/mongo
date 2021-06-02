#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.
#

# create_test_executable(target SOURCES <source files> [EXECUTABLE_NAME <name>] [BINARY_DIR <dir>] [INCLUDES <includes>]
#    [ADDITIONAL_FILES <files>] [ADDITIONAL_DIRECTORIES <dirs>] [LIBS <libs>] [FLAGS <flags>])
# Defines a C test executable binary. This helper does the necessary initialisation to ensure the correct flags and libraries
# are used when compiling the test executable.
#   target - Target name of the test.
#   SOURCES <source files> - Sources to compile for the given test.
#   EXECUTABLE_NAME <name> - A name for the output test binary. Defaults to the target name if not given.
#   BINARY_DIR <dir> - The output directory to install the binaries. Defaults to 'CMAKE_CURRENT_BINARY_DIR' if not given.
#   INCLUDES <includes> - Additional includes for building the test binary.
#   ADDITIONAL_FILES <files> - Additional files, scripts, etc we want to copy over to the output test binary. Useful if we need
#       to setup an additional wrappers needed to run the test.
#   ADDITIONAL_DIRECTORIES <dirs> - Additional directories we want to copy over to the output test binary. Useful if we need
#       to setup an additional configs and environments needed to run the test.
#   LIBS <libs> - Additional libs to link to the test binary.
#   FLAGS <flags> - Additional flags to compile the test binary with.
function(create_test_executable target)
    cmake_parse_arguments(
        PARSE_ARGV
        1
        "CREATE_TEST"
        "CXX"
        "EXECUTABLE_NAME;BINARY_DIR"
        "SOURCES;INCLUDES;ADDITIONAL_FILES;ADDITIONAL_DIRECTORIES;LIBS;FLAGS"
    )
    if (NOT "${CREATE_TEST_UNPARSED_ARGUMENTS}" STREQUAL "")
        message(FATAL_ERROR "Unknown arguments to create_test_executable: ${CREATE_TEST_UNPARSED_ARGUMENTS}")
    endif()
    if ("${CREATE_TEST_SOURCES}" STREQUAL "")
        message(FATAL_ERROR "No sources given to create_test_executable")
    endif()

    set(test_binary_dir "${CMAKE_CURRENT_BINARY_DIR}")
    # Allow the user to specify a custom binary directory.
    if(NOT "${CREATE_TEST_BINARY_DIR}" STREQUAL "")
        set(test_binary_dir "${CREATE_TEST_BINARY_DIR}")
    endif()

    # Define our test executable.
    add_executable(${target} ${CREATE_TEST_SOURCES})
    # If we want the output binary to be a different name than the target.
    if (NOT "${CREATE_TEST_EXECUTABLE_NAME}" STREQUAL "")
        set_target_properties(${target}
            PROPERTIES
            OUTPUT_NAME "${CREATE_TEST_EXECUTABLE_NAME}"
        )
    endif()
    set_target_properties(${target}
      PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${test_binary_dir}"
    )

    # Append the necessary compiler flags.
    if(NOT CREATE_TEST_CXX)
        # Don't append the strict diagnostic flags to C++ targets (as these are chosen for C targets).
        set(test_c_flags "${COMPILER_DIAGNOSTIC_FLAGS}")
    endif()
    if(NOT "${CREATE_TEST_FLAGS}" STREQUAL "")
        list(APPEND test_c_flags ${CREATE_TEST_FLAGS})
    endif()
    target_compile_options(${target} PRIVATE ${test_c_flags})

    # Include the base set of directories for a wiredtiger C test.
    target_include_directories(${target}
        PRIVATE
            ${CMAKE_SOURCE_DIR}/src/include
            ${CMAKE_SOURCE_DIR}/test/utility
            ${CMAKE_BINARY_DIR}/config
    )
    if(NOT "${CREATE_TEST_INCLUDES}" STREQUAL "")
        target_include_directories(${target} PRIVATE ${CREATE_TEST_INCLUDES})
    endif()

    # Link the base set of libraries for a wiredtiger C test.
    target_link_libraries(${target} wiredtiger test_util)
    if(NOT "${CREATE_TEST_LIBS}" STREQUAL "")
        target_link_libraries(${target} ${CREATE_TEST_LIBS})
    endif()

    # Install any additional files, scripts, etc in the output test binary
    # directory. Useful if we need to setup an additional wrappers needed to run the test
    # executable.
    foreach(file IN LISTS CREATE_TEST_ADDITIONAL_FILES)
        get_filename_component(file_basename ${file} NAME)
        # Copy the file to the given test/targets build directory.
        add_custom_command(OUTPUT ${test_binary_dir}/${file_basename}
            COMMAND ${CMAKE_COMMAND} -E copy
                ${file}
                ${test_binary_dir}/${file_basename}
            DEPENDS ${file}
        )
        add_custom_target(copy_file_${target}_${file_basename} DEPENDS ${test_binary_dir}/${file_basename})
        add_dependencies(${target} copy_file_${target}_${file_basename})
    endforeach()
    # Install any additional directories in the output test binary directory.
    # Useful if we need to setup an additional configs and environments needed to run the test executable.
    foreach(dir IN LISTS CREATE_TEST_ADDITIONAL_DIRECTORIES)
        get_filename_component(dir_basename ${dir} NAME)
        # Copy the file to the given test/targets build directory.
        add_custom_command(OUTPUT ${test_binary_dir}/${dir_basename}
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${dir}
                ${test_binary_dir}/${dir_basename}
        )
        add_custom_target(copy_dir_${target}_${dir_basename} DEPENDS ${test_binary_dir}/${dir_basename})
        add_dependencies(${target} copy_dir_${target}_${dir_basename})
    endforeach()
endfunction()
