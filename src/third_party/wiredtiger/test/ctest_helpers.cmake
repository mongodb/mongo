#[=======================================================================[
create_test_executable(target SOURCES <source files>
    [CXX] [NO_TEST_UTIL]
    [EXECUTABLE_NAME <name>]
    [BINARY_DIR <dir>]
    [INCLUDES <includes>]
    [ADDITIONAL_FILES <files>]
    [ADDITIONAL_DIRECTORIES <dirs>]
    [LIBS <libs>]
    [FLAGS <flags>]
)

Defines a C/CXX test executable binary. This helper does the necessary
initialisation to ensure the correct flags and libraries are used when
compiling the test executable.

Parameters:

target
    Target name of the test.

SOURCES <source files>
    Sources to compile for the given test.

CXX
    Indicates that this is a C++ test.
    
NO_TEST_UTIL
    Do not link against the test_util library.

EXECUTABLE_NAME <name>
    A name for the output test binary. Defaults to the target name if not given.

BINARY_DIR <dir>
    The output directory to install the binaries.
    Defaults to 'CMAKE_CURRENT_BINARY_DIR' if not given.

INCLUDES <includes>
    Additional includes for building the test binary.

ADDITIONAL_FILES <files>
    Additional files, scripts, etc we want to copy over to the output test
    binary. Useful if we need to setup an additional wrappers needed to run the
    test.

ADDITIONAL_DIRECTORIES <dirs>
    Additional directories we want to copy over to the output test binary.
    Useful if we need to setup an additional configs and environments needed to
    run the test.

LIBS <libs>
    Additional libs to link to the test binary.

FLAGS <flags>
    Additional flags to compile the test binary with.
#]=======================================================================]
function(create_test_executable target)
    cmake_parse_arguments(
        PARSE_ARGV
        1
        "CREATE_TEST"
        "CXX;NO_TEST_UTIL"
        "EXECUTABLE_NAME;BINARY_DIR"
        "SOURCES;INCLUDES;ADDITIONAL_FILES;ADDITIONAL_DIRECTORIES;LIBS;FLAGS"
    )
    if (CREATE_TEST_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "Unknown arguments to create_test_executable:
            ${CREATE_TEST_UNPARSED_ARGUMENTS}")
    endif()
    if (NOT CREATE_TEST_SOURCES)
        message(FATAL_ERROR "No sources given to create_test_executable")
    endif()

    set(test_binary_dir "${CMAKE_CURRENT_BINARY_DIR}")
    # Allow the user to specify a custom binary directory.
    if(CREATE_TEST_BINARY_DIR)
        set(test_binary_dir "${CREATE_TEST_BINARY_DIR}")
    endif()

    # Define our test executable.
    add_executable(${target} ${CREATE_TEST_SOURCES})

    # For MacOS builds we need to generate a dSYM bundle that contains the debug symbols for each
    # executable. The name of the binary will either be the name of the target or some other name
    # passed to this function. We need to use the correct one for the dsymutil.
    if (WT_DARWIN)
        if(CREATE_TEST_EXECUTABLE_NAME)
            set(test_name "${CREATE_TEST_EXECUTABLE_NAME}")
        else()
            set(test_name "${target}")
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
    if (CREATE_TEST_EXECUTABLE_NAME)
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
    if(CREATE_TEST_FLAGS)
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
    if(CREATE_TEST_INCLUDES)
        target_include_directories(${target} PRIVATE ${CREATE_TEST_INCLUDES})
    endif()

    # Link the base set of libraries for a wiredtiger C/CXX test.
    target_link_libraries(${target} wt::wiredtiger)
    if(NOT CREATE_TEST_NO_TEST_UTIL)
        target_link_libraries(${target} test_util)
    endif()
    if(CREATE_TEST_LIBS)
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
        set(copy_file_target copy_file_${target}_${file_basename})
        add_custom_target(${copy_file_target} DEPENDS ${test_binary_dir}/${file_basename})
        add_dependencies(${target} ${copy_file_target})
    endforeach()
    # Install any additional directories in the output test binary directory.
    # Useful if we need to setup an additional configs and environments needed to run the test executable.
    foreach(dir IN LISTS CREATE_TEST_ADDITIONAL_DIRECTORIES)
        get_filename_component(dir_basename ${dir} NAME)
        # Copy the directory to the given test/targets build directory.
        set(sync_dir_target sync_dir_${target}_${dir_basename})
        add_custom_target(${sync_dir_target} ALL
            COMMAND ${CMAKE_COMMAND}
                -DSYNC_DIR_SRC=${dir}
                -DSYNC_DIR_DST=${test_binary_dir}/${dir_basename}
                -P ${CMAKE_SOURCE_DIR}/test/ctest_dir_sync.cmake
        )
        add_dependencies(${target} ${sync_dir_target})
    endforeach()
endfunction()

#[=======================================================================[
define_test_variants(<test_target>
    VARIANTS <variant1> [<variant2> ...]
    [DIR_NAME <dir_name>]
    [LABELS <label1> [<label2> ...] ]
    [CMD <command>]
    [DISABLED]
)

Creates multiple test targets based on the provided variants, each with
potentially different configurations. Each variant specifies test name and the
arguments in the format: variant_name|variant args.
The test_target is used as prefix for all variants: <test_target>_<variant_name>

    Example:
        define_test_variants(test_checkpoint
            VARIANTS
                "4_mixed|-t m -T 4"
                "8_mixed|-t m -T 8"
        )

    Generated variants:
        test_checkpoint_4_mixed
        test_checkpoint_8_mixed

Parameters:

test_target
    Test executable target. It is the same name as the target in
    create_test_executable() call.

VARIANTS <variant1> [<variant2> ...]
    List of variant names to create. Each variant will generate a separate test
    target with the test arguments specified.
    Each variant should be in the format: "variant_name|variant args".

DIR_NAME <dir_name>
    Optional directory name to create for the test variant. If not specified,
    the build directory will be used.

LABELS <label1> [<label2> ...]
    Labels to apply to all test variants. These labels will be applied to
    every test variant target created.

CMD <command>
    Custom test command to use with all test variants instead of the
    given target executable.
#]=======================================================================]
function(define_test_variants target)
    cmake_parse_arguments(
        PARSE_ARGV
        1
        "DEFINE_TEST"
        ""
        "DIR_NAME;CMD"
        "VARIANTS;LABELS"
    )
    if (DEFINE_TEST_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "Unknown arguments to define_test_variants: ${DEFINE_TEST_UNPARSED_ARGUMENTS}")
    endif()
    if (NOT DEFINE_TEST_VARIANTS)
        message(FATAL_ERROR "Need at least one variant for define_test_variants")
    endif()

    set(dir_prefix)
    if(DEFINE_TEST_DIR_NAME)
        set(dir_prefix ${CMAKE_CURRENT_BINARY_DIR}/${DEFINE_TEST_DIR_NAME})
    else()
        set(dir_prefix ${CMAKE_CURRENT_BINARY_DIR})
    endif()

    set(defined_tests)
    foreach(variant_spec IN LISTS DEFINE_TEST_VARIANTS)
        # Replace '|' with ';' to convert the string into a proper CMake list
        string(REPLACE "|" ";" variant "${variant_spec}")

        list(LENGTH variant variant_length)
        if (NOT variant_length EQUAL 2)
            message(
                FATAL_ERROR
                "Invalid variant format: ${variant} - Expected format 'variant_name|variant args'"
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
        set(variant_name ${target}_${curr_variant_name})
        # Create a variant directory to run the test in.
        add_custom_command(OUTPUT ${variant_name}_test_dir
            COMMAND ${CMAKE_COMMAND} -E make_directory ${dir_prefix}/${variant_name}_test_dir
        )
        set(create_dir_target create_dir_${variant_name})
        add_custom_target(${create_dir_target} DEPENDS ${variant_name}_test_dir)
        # Ensure the variant target is created prior to building the test.
        add_dependencies(${target} ${create_dir_target})
        set(test_cmd)
        if(DEFINE_TEST_CMD)
            set(test_cmd ${DEFINE_TEST_CMD})
        else()
            set(test_cmd $<TARGET_FILE:${target}>)
        endif()
        add_test(
            NAME ${variant_name}
            COMMAND ${test_cmd} ${variant_args}
            # Run each variant in its own subdirectory, allowing us to execute variants in
            # parallel.
            WORKING_DIRECTORY ${dir_prefix}/${variant_name}_test_dir
        )
        list(APPEND defined_tests ${variant_name})
    endforeach()
    if(DEFINE_TEST_LABELS)
        set_tests_properties(${defined_tests} PROPERTIES LABELS "${DEFINE_TEST_LABELS}")
    endif()
endfunction()

#[=======================================================================[
define_c_test(
    [CXX]
    TARGET <target_name>
    SOURCES <source1> [<source2> ...]
    DIR_NAME <dir_name>
    [ARGUMENTS <argument1> [<argument2> ...] ]
    [VARIANTS <variant1> [<variant2> ...] ]
    [EXEC_SCRIPT <exec_script>]
    [LABEL <label>]
    [LIBS lib1 [lib2 ...] ]
    [FLAGS <flag1> [<flag2> ...] ]
    [DEPENDS <depend1> [<depend2> ...] ]
    [ADDITIONAL_FILES <file1> [<file2> ...] ]
)

Defines the C test with the given parameters. See test/csuite and examples/c
for examples of usage.

CXX
    Indicates that this is a C++ test.

TARGET <target_name>
    The name of the test target to create.

SOURCES <source1> [<source2> ...]
    The source files to compile for the test.

DIR_NAME <dir_name>
    The name of the directory to create for the test.

ARGUMENTS <argument1> [<argument2> ...]
    The arguments to pass to the test executable.
    Note: This is mutually exclusive with VARIANTS.

VARIANTS <variant1> [<variant2> ...]
    The test variants to create. See define_test_variants for more information.
    Note: This is mutually exclusive with ARGUMENTS.

EXEC_SCRIPT <exec_script>
    Define the C test to be executed with a script, rather than invoking
    the binary directly.

LABEL <label>
    The label to assign to the test.
    Note: Labels 'check' and 'csuite' are always assigned to generated tests.

LIBS <lib1> [<lib2> ...]
    The libraries to link against for the test executable.

DEPENDS <depend1> [<depend2> ...]
    Check that the specified csuite dependencies are enabled before compiling
    and creating the test. If the dependencies are not enabled, the test
    will not be created.

ADDITIONAL_FILES <files>
    Additional files, scripts, etc we want to copy over to the output test
    binary. Useful if we need to setup an additional wrappers needed to run the
    test.
#]=======================================================================]
function(define_c_test)
    cmake_parse_arguments(
        PARSE_ARGV
        0
        "C_TEST"
        "CXX"
        "TARGET;DIR_NAME;EXEC_SCRIPT;LABEL"
        "SOURCES;LIBS;FLAGS;ARGUMENTS;VARIANTS;DEPENDS;ADDITIONAL_FILES"
    )
    if (C_TEST_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "Unknown arguments to define_c_test: ${C_TEST_UNPARSED_ARGUMENTS}")
    endif()
    if (NOT C_TEST_TARGET)
        message(FATAL_ERROR "No target name given to define_c_test")
    endif()
    if (NOT C_TEST_SOURCES)
        message(FATAL_ERROR "No sources given to define_c_test")
    endif()
    if (NOT C_TEST_DIR_NAME)
        message(FATAL_ERROR "No directory given to define_c_test")
    endif()

    if(C_TEST_ARGUMENTS AND C_TEST_VARIANTS)
        message(FATAL_ERROR "Can't pass both ARGUMENTS and VARIANTS, use only one")
    endif()

    # Check that the csuite dependencies are enabled before compiling and creating the test.
    eval_dependency("${C_TEST_DEPENDS}" enabled)
    if(NOT enabled)
        message(WARNING "Skipping test ${C_TEST_TARGET} because dependencies "
            "${C_TEST_DEPENDS} are not enabled")
        return()
    endif()

    set(additional_executable_args)
    set(additional_file_args)
    if(C_TEST_FLAGS)
        list(APPEND additional_executable_args FLAGS ${C_TEST_FLAGS})
    endif()
    if(C_TEST_ADDITIONAL_FILES)
        list(APPEND additional_file_args ${C_TEST_ADDITIONAL_FILES})
    endif()
    if(C_TEST_CXX)
        list(APPEND additional_file_args CXX)
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
        # Define the C test to be executed with a script, rather than invoking the binary directly.
        create_test_executable(${C_TEST_TARGET}
            SOURCES ${C_TEST_SOURCES}
            LIBS ${C_TEST_LIBS}
            ADDITIONAL_FILES ${additional_file_args}
            BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/${C_TEST_DIR_NAME}
            ${additional_executable_args}
        )
        get_filename_component(exec_script_basename ${C_TEST_EXEC_SCRIPT} NAME)
        set(test_cmd ${exec_wrapper} ${exec_script_basename})
    else()
        create_test_executable(${C_TEST_TARGET}
            SOURCES ${C_TEST_SOURCES}
            LIBS ${C_TEST_LIBS}
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
        set_tests_properties(${C_TEST_TARGET} PROPERTIES LABELS "check;csuite;${C_TEST_LABEL}")
    endif()
endfunction(define_c_test)
