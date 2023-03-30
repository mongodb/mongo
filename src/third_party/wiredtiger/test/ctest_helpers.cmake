# create_test_executable(target SOURCES <source files> [EXECUTABLE_NAME <name>] [BINARY_DIR <dir>] [INCLUDES <includes>]
#    [ADDITIONAL_FILES <files>] [ADDITIONAL_DIRECTORIES <dirs>] [LIBS <libs>] [FLAGS <flags>])
# Defines a C/CXX test executable binary. This helper does the necessary initialisation to ensure the correct flags and libraries
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

    # For MacOS builds we need to generate a dSYM bundle that contains the debug symbols for each 
    # executable. The name of the binary will either be the name of the target or some other name
    # passed to this function. We need to use the correct one for the dsymutil.
    if (WT_DARWIN)
        if("${CREATE_TEST_EXECUTABLE_NAME}" STREQUAL "")
            set(test_name "${target}")
        else()
            set(test_name "${CREATE_TEST_EXECUTABLE_NAME}")
        endif()

        add_custom_command(
            TARGET ${target} POST_BUILD
            COMMAND dsymutil ${test_name}
            WORKING_DIRECTORY ${test_binary_dir}
            COMMENT "Running dsymutil on ${test_name}"
            VERBATIM
        )
    endif()

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
        set(test_flags "${COMPILER_DIAGNOSTIC_C_FLAGS}")
    else()
        set(test_flags "${COMPILER_DIAGNOSTIC_CXX_FLAGS}")
    endif()
    if(NOT "${CREATE_TEST_FLAGS}" STREQUAL "")
        list(APPEND test_flags ${CREATE_TEST_FLAGS})
    endif()
    target_compile_options(${target} PRIVATE ${test_flags})

    # Include the base set of directories for a wiredtiger C/CXX test.
    target_include_directories(${target}
        PRIVATE
            ${CMAKE_SOURCE_DIR}/src/include
            ${CMAKE_SOURCE_DIR}/test/utility
            ${CMAKE_BINARY_DIR}/config
    )
    if(NOT "${CREATE_TEST_INCLUDES}" STREQUAL "")
        target_include_directories(${target} PRIVATE ${CREATE_TEST_INCLUDES})
    endif()

    # Link the base set of libraries for a wiredtiger C/CXX test.
    target_link_libraries(${target} wt::wiredtiger test_util)
    if(NOT "${CREATE_TEST_LIBS}" STREQUAL "")
        target_link_libraries(${target} ${CREATE_TEST_LIBS})
    endif()

    if(ENABLE_ANTITHESIS)
        target_link_libraries(${target} wt::voidstar)
    endif()

    if(WT_LINUX OR WT_DARWIN)
        # Link the final test executable with a relative runpath to the
        # top-level build directory. This being the build location of the
        # WiredTiger library.
        set(origin_linker_variable)
        if(WT_LINUX)
            set(origin_linker_variable "\\$ORIGIN")
        elseif(WT_DARWIN)
            set(origin_linker_variable "@loader_path")
        endif()
        file(RELATIVE_PATH relative_rpath ${test_binary_dir} ${CMAKE_BINARY_DIR})
        set_target_properties(${target}
            PROPERTIES
                # Setting this variable to false adds the relative path to the list of RPATH.
                BUILD_WITH_INSTALL_RPATH FALSE
                LINK_FLAGS "-Wl,-rpath,${origin_linker_variable}/${relative_rpath}"
        )
    endif()

    if (NOT WT_WIN)
        target_link_libraries(${target} m)
    endif()

    # If compiling for windows, additionally link in the shim library.
    if(WT_WIN)
        target_include_directories(
            ${target}
            PUBLIC ${CMAKE_SOURCE_DIR}/test/windows
        )
        target_link_libraries(${target} windows_shim)
    endif()

    if(ENABLE_TCMALLOC)
        # Order matters on linking as WT lib depends on tcmalloc when enabled.
        target_link_libraries(${target} wt::wiredtiger wt::tcmalloc)
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
        # Copy the directory to the given test/targets build directory.
        add_custom_target(sync_dir_${target}_${dir_basename} ALL
            COMMAND ${CMAKE_COMMAND}
                -DSYNC_DIR_SRC=${dir}
                -DSYNC_DIR_DST=${test_binary_dir}/${dir_basename}
                -P ${CMAKE_SOURCE_DIR}/test/ctest_dir_sync.cmake
        )
        add_dependencies(${target} sync_dir_${target}_${dir_basename})
    endforeach()
endfunction()

function(define_test_variants target)
    cmake_parse_arguments(
        PARSE_ARGV
        1
        "DEFINE_TEST"
        ""
        "DIR_NAME"
        "VARIANTS;LABELS;CMDS"
    )
    if (NOT "${DEFINE_TEST_UNPARSED_ARGUMENTS}" STREQUAL "")
        message(FATAL_ERROR "Unknown arguments to define_test_variants: ${DEFINE_TEST_UNPARSED_ARGUMENTS}")
    endif()
    if ("${DEFINE_TEST_VARIANTS}" STREQUAL "")
        message(FATAL_ERROR "Need at least one variant for define_test_variants")
    endif()

    set(dir_prefix)
    if(DEFINE_TEST_DIR_NAME)
        set(dir_prefix ${CMAKE_CURRENT_BINARY_DIR}/${DEFINE_TEST_DIR_NAME})
    else()
        set(dir_prefix ${CMAKE_CURRENT_BINARY_DIR})
    endif()

    set(defined_tests)
    foreach(variant ${DEFINE_TEST_VARIANTS})
        list(LENGTH variant variant_length)
        if (NOT variant_length EQUAL 2)
            message(
                FATAL_ERROR
                "Invalid variant format: ${variant} - Expected format 'variant_name;variant args'"
            )
        endif()
        list(GET variant 0 curr_variant_name)
        list(GET variant 1 curr_variant_args)
        set(variant_args)
        if(WT_WIN)
            separate_arguments(variant_args WINDOWS_COMMAND ${curr_variant_args})
        else()
            separate_arguments(variant_args UNIX_COMMAND ${curr_variant_args})
        endif()
        # Create a variant directory to run the test in.
        add_custom_command(OUTPUT ${curr_variant_name}_test_dir
            COMMAND ${CMAKE_COMMAND} -E make_directory ${dir_prefix}/${curr_variant_name}_test_dir
        )
        add_custom_target(create_dir_${curr_variant_name} DEPENDS ${curr_variant_name}_test_dir)
        # Ensure the variant target is created prior to building the test.
        add_dependencies(${target} create_dir_${curr_variant_name})
        set(test_cmd)
        if(DEFINE_TEST_CMDS)
            set(test_cmd ${DEFINE_TEST_CMDS})
        else()
            set(test_cmd $<TARGET_FILE:${target}>)
        endif()
        add_test(
            NAME ${curr_variant_name}
            COMMAND ${test_cmd} ${variant_args}
            # Run each variant in its own subdirectory, allowing us to execute variants in
            # parallel.
            WORKING_DIRECTORY ${dir_prefix}/${curr_variant_name}_test_dir
        )
        list(APPEND defined_tests ${curr_variant_name})
    endforeach()
    if(DEFINE_TEST_LABELS)
        set_tests_properties(${defined_tests} PROPERTIES LABELS "${DEFINE_TEST_LABELS}")
    endif()
endfunction()

function(define_c_test)
    cmake_parse_arguments(
        PARSE_ARGV
        0
        "C_TEST"
        ""
        "TARGET;DIR_NAME;EXEC_SCRIPT"
        "SOURCES;FLAGS;ARGUMENTS;VARIANTS;DEPENDS;ADDITIONAL_FILES"
    )
    if (NOT "${C_TEST_UNPARSED_ARGUMENTS}" STREQUAL "")
        message(FATAL_ERROR "Unknown arguments to define_c_test: ${C_TEST_UNPARSED_ARGUMENTS}")
    endif()
    if ("${C_TEST_TARGET}" STREQUAL "")
        message(FATAL_ERROR "No target name given to define_c_test")
    endif()
    if ("${C_TEST_SOURCES}" STREQUAL "")
        message(FATAL_ERROR "No sources given to define_c_test")
    endif()
    if ("${C_TEST_DIR_NAME}" STREQUAL "")
        message(FATAL_ERROR "No directory given to define_c_test")
    endif()

    if("${C_TEST_ARGUMENTS}" AND "${C_TEST_VARIANTS}")
        message(FATAL_ERROR "Can't pass both ARGUMENTS and VARIANTS, use only one")
    endif()

    # Check that the csuite dependencies are enabled before compiling and creating the test.
    eval_dependency("${C_TEST_DEPENDS}" enabled)
    if(enabled)
        set(additional_executable_args)
        set(additional_file_args)
        if(NOT "${C_TEST_FLAGS}" STREQUAL "")
            list(APPEND additional_executable_args FLAGS ${C_TEST_FLAGS})
        endif()
        if(NOT "${C_TEST_ADDITIONAL_FILES}" STREQUAL "")
            list(APPEND additional_file_args ${C_TEST_ADDITIONAL_FILES})
        endif()
        set(exec_wrapper)
        if(WT_WIN)
            # This is a workaround to run our csuite tests under Windows using CTest. When executing a test,
            # CTests by-passes the shell and directly executes the test as a child process. In doing so CTest executes the binary with forward-slash paths.
            # Which while technically valid breaks assumptions in our testing utilities. Wrap the execution in powershell to avoid this.
            set(exec_wrapper "powershell.exe")
        endif()
        set(test_cmd)
        if (C_TEST_EXEC_SCRIPT)
            list(APPEND additional_file_args ${C_TEST_EXEC_SCRIPT})
            # Define the c test to be executed with a script, rather than invoking the binary directly.
            create_test_executable(${C_TEST_TARGET}
                SOURCES ${C_TEST_SOURCES}
                ADDITIONAL_FILES ${additional_file_args}
                BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/${C_TEST_DIR_NAME}
                ${additional_executable_args}
            )
            get_filename_component(exec_script_basename ${C_TEST_EXEC_SCRIPT} NAME)
            set(test_cmd ${exec_wrapper} ${exec_script_basename})
        else()
            create_test_executable(${C_TEST_TARGET}
                SOURCES ${C_TEST_SOURCES}
                ADDITIONAL_FILES ${additional_file_args}
                BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/${C_TEST_DIR_NAME}
                ${additional_executable_args}
            )
            set(test_cmd ${exec_wrapper} $<TARGET_FILE:${C_TEST_TARGET}>)
        endif()
        # Define the ctest target.
        if(C_TEST_VARIANTS)
            # If we want to define multiple variant executions of the test script/binary.
            define_test_variants(${C_TEST_TARGET}
                VARIANTS ${C_TEST_VARIANTS}
                CMDS ${test_cmd}
                DIR_NAME ${C_TEST_DIR_NAME}
                LABELS "check;csuite"
            )
        else()
            add_test(NAME ${C_TEST_TARGET}
                COMMAND ${test_cmd} ${C_TEST_ARGUMENTS}
                WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/${C_TEST_DIR_NAME}
            )
            set_tests_properties(${C_TEST_TARGET} PROPERTIES LABELS "check;csuite")
        endif()
    endif()
endfunction(define_c_test)
