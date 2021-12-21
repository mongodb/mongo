#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#  All rights reserved.
#
#  See the file LICENSE for redistribution information
#

include(CheckIncludeFiles)
include(CheckSymbolExists)
include(CheckLibraryExists)
include(CheckTypeSize)

# Helper function for evaluating a list of dependencies. Mostly used by the
# "config_X" helpers to evaluate the dependencies required to enable the config
# option.
#   depends - a list (semicolon seperated) of dependencies to evaluate.
#   enabled - name of the output variable set with either 'ON' or 'OFF' (based
#             on evaluated dependencies). Output variable is set in the callers scope.
function(eval_dependency depends enabled)
    # If no dependencies were given then we default to an enabled state.
    if(("${depends}" STREQUAL "") OR ("${depends}" STREQUAL "NOTFOUND"))
        set(enabled ON PARENT_SCOPE)
        return()
    endif()
    # Evaluate each dependency.
    set(is_enabled ON)
    foreach(dependency ${depends})
        string(REGEX REPLACE " +" ";" dependency "${dependency}")
        if(NOT (${dependency}))
            set(is_enabled OFF)
            break()
        endif()
    endforeach()
    set(enabled ${is_enabled} PARENT_SCOPE)
endfunction()

# config_string(config_name description DEFAULT <default string> [DEPENDS <deps>] [INTERNAL])
# Defines a string configuration option. The configuration option is stored in the cmake cache
# and can be exported to the wiredtiger config header.
#   config_name - name of the configuration option.
#   description - docstring to describe the configuration option (viewable in the cmake-gui).
#   DEFAULT <default string> -  Default value of the configuration string. Used when not manually set
#       by a cmake script or in the cmake-gui.
#   DEPENDS <deps> - list of dependencies (semicolon seperated) required for the configuration string
#       to be present and set in the cache. If any of the dependencies aren't met, the
#       configuration value won't be present in the cache.
#   INTERNAL - hides the configuration option from the cmake-gui by default. Useful if you don't want
#       to expose the variable by default to the user i.e keep it internal to the implementation
#       (but still need it in the cache).
function(config_string config_name description)
    cmake_parse_arguments(
        PARSE_ARGV
        2
        "CONFIG_STR"
        "INTERNAL"
        "DEFAULT;DEPENDS"
        ""
    )

    if (NOT "${CONFIG_STR_UNPARSED_ARGUMENTS}" STREQUAL "")
        message(FATAL_ERROR "Unknown arguments to config_str: ${CONFIG_STR_UNPARSED_ARGUMENTS}")
    endif()

    # Check that the configs dependencies are enabled before setting it to a visible enabled state.
    eval_dependency("${CONFIG_STR_DEPENDS}" enabled)
    set(default_value "${CONFIG_STR_DEFAULT}")
    if(enabled)
        # Set an internal cache variable "${config_name}_DISABLED" to capture its enabled/disabled state
        # We want to ensure we capture a transition from a disabled to enabled state when dependencies are met.
        if(${config_name}_DISABLED)
            unset(${config_name}_DISABLED CACHE)
            set(${config_name} ${default_value} CACHE STRING "${description}" FORCE)
        else()
            set(${config_name} ${default_value} CACHE STRING "${description}")
        endif()
        if (CONFIG_STR_INTERNAL)
            # Mark as an advanced variable, hiding it from initial UI's views.
            mark_as_advanced(FORCE ${config_name})
        endif()
    else()
        # Config doesn't meet dependency requirements, remove it from the cache and flag it as disabled.
        unset(${config_name} CACHE)
        set(${config_name}_DISABLED ON CACHE INTERNAL "" FORCE)
    endif()
endfunction()

# config_choice(config_name description OPTIONS <opts>)
# Defines a configuration option, bounded with pre-set toggleable values. The configuration option is stored
# in the cmake cache and can be exported to the wiredtiger config header. We default to the first *available* option in the
# list if the config has not been manually set by a cmake script or in the cmake-gui.
#   config_name - name of the configuration option.
#   description - docstring to describe the configuration option (viewable in the cmake-gui).
#   OPTIONS - a list of option values that the configuration option can be set to. Each option is itself a semicolon
#       seperated list consisting of "<option-name>;<config-name>;<option-dependencies>".
#       * option-name: name of the given option stored in the ${config_name} cache variable and presented
#           to users in the gui (usually something understandable).
#       * config-name: an additional cached configuration variable that is made available if the option is selected.
#           It is only present if the option is chosen, otherwise it is unset.
#       *  option-dependencies: dependencies required for the option to be made available. If its dependencies aren't met
#           the given option will become un-selectable.
function(config_choice config_name description)
    cmake_parse_arguments(
        PARSE_ARGV
        2
        "CONFIG_OPT"
        ""
        ""
        "OPTIONS"
    )

    if (NOT "${CONFIG_OPT_UNPARSED_ARGUMENTS}" STREQUAL "")
        message(FATAL_ERROR "Unknown arguments to config_opt: ${CONFIG_OPT_UNPARSED_ARGUMENTS}")
    endif()
    # We require option values (not optional)
    if ("${CONFIG_OPT_OPTIONS}" STREQUAL "")
        message(FATAL_ERROR "No options passed")
    endif()

    set(found_option ON)
    set(found_pre_set OFF)
    set(default_config_field "")
    set(default_config_var "")
    foreach(curr_option ${CONFIG_OPT_OPTIONS})
        list(LENGTH curr_option opt_length)
        if (NOT opt_length EQUAL 3)
            message(FATAL_ERROR "Invalid option format: ${curr_option}")
        endif()
        # We expect three items defined for each option.
        list(GET curr_option 0 option_config_field)
        list(GET curr_option 1 option_config_var)
        list(GET curr_option 2 option_depends)
        # Check that the options dependencies are enabled before setting it to a selectable state.
        eval_dependency("${option_depends}" enabled)
        if(enabled)
            list(APPEND all_option_config_fields ${option_config_field})
            # The first valid/selectable option found will be the default config value.
            if (found_option)
                set(found_option OFF)
                set(default_config_field "${option_config_field}")
                set(default_config_var "${option_config_var}")
            endif()
            # Check if the option is already set with this given field. We don't want to override the configs value
            # with a default value if its already been pre-set in the cache e.g. by early config scripts.
            if("${${config_name}}" STREQUAL "${option_config_field}")
                set(${option_config_var} ON CACHE INTERNAL "" FORCE)
                set(${config_name}_CONFIG_VAR ${option_config_var} CACHE INTERNAL "" FORCE)
                set(found_pre_set ON)
                set(found_option OFF)
                set(default_config_field "${option_config_field}")
                set(default_config_var "${option_config_var}")
            else()
                # Clear the cache of the current set value.
                set(${option_config_var} OFF CACHE INTERNAL "" FORCE)
            endif()
        else()
            unset(${option_config_var} CACHE)
            # Check if the option is already set with this given field - we want to clear it if so.
            if ("${${config_name}_CONFIG_VAR}" STREQUAL "${option_config_var}")
                unset(${config_name}_CONFIG_VAR CACHE)
            endif()
            if("${${config_name}}" STREQUAL "${option_config_field}")
                unset(${config_name} CACHE)
            endif()
        endif()
    endforeach()
    # If the config hasn't been set we can load it with the default option found earlier.
    if(NOT found_pre_set)
        set(${default_config_var} ON CACHE INTERNAL "" FORCE)
        set(${config_name} ${default_config_field} CACHE STRING ${description})
        set(${config_name}_CONFIG_VAR ${default_config_var} CACHE INTERNAL "" FORCE)
    endif()
    set_property(CACHE ${config_name} PROPERTY STRINGS ${all_option_config_fields})
endfunction()

# config_bool(config_name description DEFAULT <default-value> [DEPENDS <deps>] [DEPENDS_ERROR <config-val> <error-string>])
# Defines a boolean (ON/OFF) configuration option . The configuration option is stored in the cmake cache
# and can be exported to the wiredtiger config header.
#   config_name - name of the configuration option.
#   description - docstring to describe the configuration option (viewable in the cmake-gui).
#   DEFAULT <default-value> -  default value of the configuration bool (ON/OFF). Used when not manually set
#       by a cmake script or in the cmake-gui or when dependencies aren't met.
#   DEPENDS <deps> - list of dependencies (semicolon seperated) required for the configuration bool
#       to be set to the desired value. If any of the dependencies aren't met the configuration value
#       will be set to its default value.
#   DEPENDS_ERROR <config-val> <error-string> - specifically throw a fatal error when the configuration option is set to
#       <config-val> despite failing on its dependencies. This is mainly used for commandline-like options where you want
#       to signal a specific error to the caller when dependencies aren't met e.g. toolchain is missing library (as opposed to
#       silently defaulting).
function(config_bool config_name description)
    cmake_parse_arguments(
        PARSE_ARGV
        2
        "CONFIG_BOOL"
        ""
        "DEFAULT;DEPENDS"
        "DEPENDS_ERROR"
    )

    if(NOT "${CONFIG_BOOL_UNPARSED_ARGUMENTS}" STREQUAL "")
        message(FATAL_ERROR "Unknown arguments to config_bool: ${CONFIG_BOOL_UNPARSED_ARGUMENTS}")
    endif()
    # We require a default value (not optional).
    if("${CONFIG_BOOL_DEFAULT}" STREQUAL "")
        message(FATAL_ERROR "No default value passed")
    endif()

    set(depends_err_value)
    set(depends_err_message "")
    # If DEPENDS_ERROR is specifically set, parse the value we want to throw an error on if the dependency fails.
    if(CONFIG_BOOL_DEPENDS_ERROR)
        list(LENGTH CONFIG_BOOL_DEPENDS_ERROR depends_error_length)
        if(NOT depends_error_length EQUAL 2)
            message(FATAL_ERROR "Invalid usage of DEPENDS_ERROR: requires <Error Value> <Error Message>")
        else()
            list(GET CONFIG_BOOL_DEPENDS_ERROR 0 err_val)
            if(err_val)
                set(depends_err_value "1")
            else()
                set(depends_err_value "0")
            endif()
            list(GET CONFIG_BOOL_DEPENDS_ERROR 1 depends_err_message)
        endif()
    endif()

    # Check that the configs dependencies are enabled before setting it to a visible enabled state.
    eval_dependency("${CONFIG_BOOL_DEPENDS}" enabled)
    if(enabled)
        # Set an internal cache variable "${config_name}_DISABLED" to capture its enabled/disabled state.
        # We want to ensure we capture a transition from a disabled to enabled state when dependencies are met.
        if(${config_name}_DISABLED)
            unset(${config_name}_DISABLED CACHE)
            set(${config_name} ${CONFIG_BOOL_DEFAULT} CACHE STRING "${description}" FORCE)
        else()
            set(${config_name} ${CONFIG_BOOL_DEFAULT} CACHE STRING "${description}")
        endif()
    else()
        set(config_value "0")
        if (${${config_name}})
            set(config_value "1")
        endif()
        # If the user tries to set the config option to a given value when its dependencies
        # are not met, throw an error (when DEPENDS_ERROR is explicitly set).
        if(CONFIG_BOOL_DEPENDS_ERROR)
            if(${depends_err_value} EQUAL ${config_value})
                message(FATAL_ERROR "Unable to set ${config_name}: ${depends_err_message}")
            endif()
        endif()
        # Config doesn't meet dependency requirements, set its default state and flag it as disabled.
        set(${config_name} OFF CACHE STRING "${description}" FORCE)
        set(${config_name}_DISABLED ON CACHE INTERNAL "" FORCE)
    endif()
endfunction()

# config_func(config_name description FUNC <function-symbol> FILE <include-header> [DEPENDS <deps>] [LIBS <library-dependencies>])
# Defines a boolean (0/1) configuration option based on whether a given function symbol exists.
# The configuration option is stored in the cmake cache and can be exported to the wiredtiger config header.
#   config_name - name of the configuration option.
#   description - docstring to describe the configuration option (viewable in the cmake-gui).
#   FUNC <function-symbol> - function symbol we want to search for.
#   FILE <include-header> - header we expect the function symbol to be defined e.g a std header.
#   DEPENDS <deps> - list of dependencies (semicolon seperated) required for the configuration to be evaluated.
#       If any of the dependencies aren't met the configuration value will be set to '0' (false).
#   LIBS <library-dependencies> - a list of any additional library dependencies needed to successfully link with the function symbol.
function(config_func config_name description)
    cmake_parse_arguments(
        PARSE_ARGV
        2
        "CONFIG_FUNC"
        ""
        "FUNC;DEPENDS;FILES;LIBS"
        ""
    )

    if (NOT "${CONFIG_FUNC_UNPARSED_ARGUMENTS}" STREQUAL "")
        message(FATAL_ERROR "Unknown arguments to config_func: ${CONFIG_FUNC_UNPARSED_ARGUMENTS}")
    endif()
    # We require an include header (not optional).
    if ("${CONFIG_FUNC_FILES}" STREQUAL "")
        message(FATAL_ERROR "No file list passed")
    endif()
    # We require a function symbol (not optional).
    if ("${CONFIG_FUNC_FUNC}" STREQUAL "")
        message(FATAL_ERROR "No function passed")
    endif()

    # Check that the configs dependencies are enabled before setting it to a visible enabled state.
    eval_dependency("${CONFIG_FUNC_DEPENDS}" enabled)
    if(enabled)
        set(CMAKE_REQUIRED_LIBRARIES "${CONFIG_FUNC_LIBS}")
        if((NOT "${WT_ARCH}" STREQUAL "") AND (NOT "${WT_ARCH}" STREQUAL ""))
            # 'check_symbol_exists' won't use our current cache when test compiling the function symbol.
            # To get around this we need to ensure we manually forward WT_ARCH and WT_OS as a minimum. This is particularly
            # needed if 'check_symbol_exists' will leverage one of our toolchain files.
            set(CMAKE_REQUIRED_FLAGS "-DWT_ARCH=${WT_ARCH} -DWT_OS=${WT_OS}")
        endif()
        check_symbol_exists(${CONFIG_FUNC_FUNC} "${CONFIG_FUNC_FILES}" has_symbol_${config_name})
        set(CMAKE_REQUIRED_LIBRARIES)
        set(CMAKE_REQUIRED_FLAGS)
        set(has_symbol "0")
        if(has_symbol_${config_name})
            set(has_symbol ${has_symbol_${config_name}})
        endif()
        # Set an internal cache variable "${config_name}_DISABLED" to capture its enabled/disabled state.
        # We want to ensure we capture a transition from a disabled to enabled state when dependencies are met.
        if(${config_name}_DISABLED)
            unset(${config_name}_DISABLED CACHE)
            set(${config_name} ${has_symbol} CACHE STRING "${description}" FORCE)
        else()
            set(${config_name} ${has_symbol} CACHE STRING "${description}")
        endif()
        # 'check_symbol_exists' sets our given temp variable into the cache. Clear this so it doesn't persist between
        # configuration runs.
        unset(has_symbol_${config_name} CACHE)
    else()
        # Config doesn't meet dependency requirements, set a disabled state.
        set(${config_name} 0 CACHE INTERNAL "" FORCE)
        set(${config_name}_DISABLED ON CACHE INTERNAL "" FORCE)
    endif()
endfunction()


# config_include(config_name description FILE <include-header> [DEPENDS <deps>])
# Defines a boolean (0/1) configuration option based on whether a given include header exists.
# The configuration option is stored in the cmake cache and can be exported to the wiredtiger config header.
#   config_name - name of the configuration option.
#   description - docstring to describe the configuration option (viewable in the cmake-gui).
#   FILE <include-header> - header we want to search for e.g a std header.
#   DEPENDS <deps> - list of dependencies (semicolon seperated) required for the configuration to be evaluated.
#       If any of the dependencies aren't met the configuration value will be set to '0' (false).
function(config_include config_name description)
    cmake_parse_arguments(
        PARSE_ARGV
        2
        "CONFIG_INCLUDE"
        ""
        "FILE;DEPENDS"
        ""
    )

    if (NOT "${CONFIG_INCLUDE_UNPARSED_ARGUMENTS}" STREQUAL "")
        message(FATAL_ERROR "Unknown arguments to config_include: ${CONFIG_INCLUDE_UNPARSED_ARGUMENTS}")
    endif()
    # We require a include header (not optional).
    if ("${CONFIG_INCLUDE_FILE}" STREQUAL "")
        message(FATAL_ERROR "No include file passed")
    endif()

    # Check that the configs dependencies are enabled before setting it to a visible enabled state.
    eval_dependency("${CONFIG_INCLUDE_DEPENDS}" enabled)
    if(enabled)
        # 'check_include_files' won't use our current cache when test compiling the include header.
        # To get around this we need to ensure we manually forward WT_ARCH and WT_OS as a minimum. This is particularly
        # needed if 'check_include_files' will leverage one of our toolchain files.
        if((NOT "${WT_ARCH}" STREQUAL "") AND (NOT "${WT_ARCH}" STREQUAL ""))
            set(CMAKE_REQUIRED_FLAGS "-DWT_ARCH=${WT_ARCH} -DWT_OS=${WT_OS}")
        endif()
        check_include_files(${CONFIG_INCLUDE_FILE} has_include_${config_name})
        set(CMAKE_REQUIRED_FLAGS)
        set(has_include "0")
        if(has_include_${config_name})
            set(has_include ${has_include_${config_name}})
        endif()
        # Set an internal cache variable "${config_name}_DISABLED" to capture its enabled/disabled state.
        # We want to ensure we capture a transition from a disabled to enabled state when dependencies are met.
        if(${config_name}_DISABLED)
            unset(${config_name}_DISABLED CACHE)
            set(${config_name} ${has_include} CACHE STRING "${description}" FORCE)
        else()
            set(${config_name} ${has_include} CACHE STRING "${description}")
        endif()
        # 'check_include_files' sets our given temp variable into the cache. Clear this so it doesn't persist between
        # configuration runs.
        unset(has_include_${config_name} CACHE)
    else()
        set(${config_name} OFF CACHE INTERNAL "" FORCE)
        set(${config_name}_DISABLED ON CACHE INTERNAL "" FORCE)
    endif()
    # Set an internal cache variable with the CPP include statement. We can use this when building out our config header.
    if (${${config_name}})
        set(${config_name}_DECL "#include <${CONFIG_INCLUDE_FILE}>" CACHE INTERNAL "")
    endif()
endfunction()

# config_lib(config_name description LIB <library> FUNC <function-symbol> [DEPENDS <deps>] [HEADER <file>])
# Defines a boolean (0/1) configuration option based on whether a given library exists.
# The configuration option is stored in the cmake cache and can be exported to the wiredtiger config header.
#   config_name - name of the configuration option.
#   description - docstring to describe the configuration option (viewable in the cmake-gui).
#   LIB <library> - library we are searching for (defined as if we are linking against it e.g -lpthread).
#   FUNC <function-symbol> - function symbol we expect to be available to link against within the library.
#   DEPENDS <deps> - list of dependencies (semicolon seperated) required for the configuration to be evaluated.
#       If any of the dependencies aren't met the configuration value will be set to '0' (false).
function(config_lib config_name description)
    cmake_parse_arguments(
        PARSE_ARGV
        2
        "CONFIG_LIB"
        ""
        "LIB;DEPENDS;HEADER"
        ""
    )

    if (NOT "${CONFIG_LIB_UNPARSED_ARGUMENTS}" STREQUAL "")
        message(FATAL_ERROR "Unknown arguments to config_lib: ${CONFIG_LIB_UNPARSED_ARGUMENTS}")
    endif()
    # We require a library (not optional).
    if ("${CONFIG_LIB_LIB}" STREQUAL "")
        message(FATAL_ERROR "No library passed")
    endif()

    # Check that the configs dependencies are enabled before setting it to a visible enabled state.
    eval_dependency("${CONFIG_LIB_DEPENDS}" enabled)
    if(enabled)
        message("-- Looking for library ${CONFIG_LIB_LIB}")
        # 'check_library_exists' won't use our current cache when test compiling the library.
        # To get around this we need to ensure we manually forward WT_ARCH and WT_OS as a minimum. This is particularly
        # needed if 'check_library_exists' will leverage one of our toolchain files.
        if((NOT "${WT_ARCH}" STREQUAL "") AND (NOT "${WT_ARCH}" STREQUAL ""))
            set(CMAKE_REQUIRED_FLAGS "-DWT_ARCH=${WT_ARCH} -DWT_OS=${WT_OS}")
        endif()
        find_library(has_lib_${config_name} ${CONFIG_LIB_LIB})
        set(CMAKE_REQUIRED_FLAGS)
        set(has_lib "0")
        set(has_include "")
        if(has_lib_${config_name})
            set(has_lib ${has_lib_${config_name}})
            if (CONFIG_LIB_HEADER)
                find_path(include_path_${config_name} ${CONFIG_LIB_HEADER})
                if (include_path_${config_name})
                    message("-- Looking for library ${CONFIG_LIB_LIB}: found ${has_lib_${config_name}}, include path ${include_path_${config_name}}")
                    set(has_include ${include_path_${config_name}})
                else()
                    message("-- Looking for library ${CONFIG_LIB_LIB}: found ${has_lib_${config_name}}")
                endif()
                unset(include_path_${config_name} CACHE)
            else()
                message("-- Looking for library ${CONFIG_LIB_LIB}: found ${has_lib_${config_name}}")
            endif()
        else()
            message("-- Looking for library ${CONFIG_LIB_LIB}: NOT found")
        endif()
        # Set an internal cache variable "${config_name}_DISABLED" to capture its enabled/disabled state.
        # We want to ensure we capture a transition from a disabled to enabled state when dependencies are met.
        if(${config_name}_DISABLED)
            unset(${config_name}_DISABLED CACHE)
            set(${config_name} ${has_lib} CACHE STRING "${description}" FORCE)
            set(${config_name}_INCLUDES ${has_include} CACHE STRING "Additional include paths for ${config_name}" FORCE)
        else()
            set(${config_name} ${has_lib} CACHE STRING "${description}")
            set(${config_name}_INCLUDES ${has_include} CACHE STRING "Additional include paths for ${config_name}")
        endif()
        # 'check_library_exists' sets our given temp variable into the cache. Clear this so it doesn't persist between
        # configuration runs.
        unset(has_lib_${config_name} CACHE)
    else()
        message("-- Not looking for library ${CONFIG_LIB_LIB}: disabled")
        set(${config_name} 0 CACHE INTERNAL "" FORCE)
        set(${config_name}_DISABLED ON CACHE INTERNAL "" FORCE)
    endif()
endfunction()

# config_compile(config_name description SOURCE <source-file> [DEPENDS <deps>] [LIBS <library-dependencies>])
# Defines a boolean (0/1) configuration option based on whether a source file can be successfully compiled and run. Used
# to determine if more fine grained functionality is supported on a given target environment (beyond what function
# symbols, libraries and headers are available). The configuration option is stored in the cmake cache and can be
# exported to the wiredtiger config header.
#   config_name - name of the configuration option.
#   description - docstring to describe the configuration option (viewable in the cmake-gui).
#   SOURCE <source-file> - specific source file we want to test compile.
#   DEPENDS <deps> - list of dependencies (semicolon seperated) required for the configuration to be evaluated.
#       If any of the dependencies aren't met the configuration value will be set to '0' (false).
#   LIBS <library-dependencies> - a list of any additional library dependencies needed to successfully compile the source.
function(config_compile config_name description)
    cmake_parse_arguments(
        PARSE_ARGV
        2
        "CONFIG_COMPILE"
        ""
        "SOURCE;DEPENDS;LIBS"
        ""
    )

    if (NOT "${CONFIG_COMPILE_UNPARSED_ARGUMENTS}" STREQUAL "")
        message(FATAL_ERROR "Unknown arguments to config_compile: ${CONFIG_COMPILE_UNPARSED_ARGUMENTS}")
    endif()
    # We require a source file (not optional).
    if ("${CONFIG_COMPILE_SOURCE}" STREQUAL "")
        message(FATAL_ERROR "No source passed")
    endif()

    # Check that the configs dependencies are enabled before setting it to a visible enabled state.
    eval_dependency("${CONFIG_COMPILE_DEPENDS}" enabled)
    if(enabled)
        # Test compile the source file.
        try_run(
            can_run_${config_name} can_compile_${config_name}
            ${CMAKE_CURRENT_BINARY_DIR}
            ${CONFIG_COMPILE_SOURCE}
            CMAKE_FLAGS "-DWT_ARCH=${WT_ARCH}" "-DWT_OS=${WT_OS}"
            LINK_LIBRARIES "${CONFIG_COMPILE_LIBS}"
        )
        set(can_run "0")
        if((NOT "${can_run_${config_name}}" STREQUAL "FAILED_TO_RUN") AND
            ("${can_run_${config_name}}" STREQUAL "0"))
            set(can_run "1")
        endif()
        # Set an internal cache variable "${config_name}_DISABLED" to capture its enabled/disabled state.
        # We want to ensure we capture a transition from a disabled to enabled state when dependencies are met.
        if(${config_name}_DISABLED)
            unset(${config_name}_DISABLED CACHE)
            set(${config_name} ${can_run} CACHE STRING "${description}" FORCE)
        else()
            set(${config_name} ${can_run} CACHE STRING "${description}")
        endif()
        # 'try_run' sets our given temp variable into the cache. Clear this so it doesn't persist between
        # configuration runs.
        unset(can_run_${config_name} CACHE)
        unset(can_compile_${config_name} CACHE)
    else()
        set(${config_name} 0 CACHE INTERNAL "" FORCE)
        set(${config_name}_DISABLED ON CACHE INTERNAL "" FORCE)
    endif()
endfunction()

# test_type_size(type output_size)
# Helper function that tests for a given types size and returns its value if found.
#   type - name of the type to test.
#   output_size - name of the output variable, set with either the types size or "" (empty string)
#       if not found.
#   EXTRA_INCLUDES - extra/optional include files to access the given type e.g. a custom typedef in an include header.
function(test_type_size type output_size)
    cmake_parse_arguments(
        PARSE_ARGV
        2
        "TEST_TYPE"
        ""
        ""
        "EXTRA_INCLUDES"
    )

    if (NOT "${TEST_TYPE_UNPARSED_ARGUMENTS}" STREQUAL "")
        message(FATAL_ERROR "Unknown arguments to assert_type: ${TEST_TYPE_UNPARSED_ARGUMENTS}")
    endif()

    set(CMAKE_EXTRA_INCLUDE_FILES "${TEST_TYPE_EXTRA_INCLUDES}")
    check_type_size(${type} TEST_TYPE)
    set(CMAKE_EXTRA_INCLUDE_FILES)

    if(NOT HAVE_TEST_TYPE)
        set(${output_size} "" PARENT_SCOPE)
    else()
        set(${output_size} ${TEST_TYPE} PARENT_SCOPE)
    endif()
endfunction()

# assert_type_size(type size)
# Wrapper function around 'test_type_size' that additionally asserts whether the given types meets an expected size.
# Throws a fatal error if the type is not found or doesn't equal the expected size.
#   type - name of the type to test.
#   size - expected size of the type.
#   EXTRA_INCLUDES - extra/optional include files to access the given type e.g. a custom typedef in an include header.
function(assert_type_size type size)
    cmake_parse_arguments(
        PARSE_ARGV
        2
        "ASSERT_TYPE"
        ""
        ""
        "EXTRA_INCLUDES"
    )

    if (NOT "${ASSERT_TYPE_UNPARSED_ARGUMENTS}" STREQUAL "")
        message(FATAL_ERROR "Unknown arguments to assert_type: ${ASSERT_TYPE_UNPARSED_ARGUMENTS}")
    endif()

    set(additional_args "")
    if(${ASSERT_TYPE_EXTRA_INCLUDES})
        set(additional_args "EXTRA_INCLUDES ${ASSERT_TYPE_EXTRA_INCLUDES}")
    endif()
    test_type_size(${type} output_type_size ${additional_args})

    if(${output_type_size} EQUAL "")
        # Type does not exist.
        message(FATAL_ERROR "Type assertion failed: ${type} does not exists")
    endif()

    if((NOT ${size} EQUAL 0) AND (NOT ${output_type_size} EQUAL ${size}))
        # Type does not meet size assertion.
        message(FATAL_ERROR "Type assertion failed: ${type} does not equal size ${size}")
    endif()
endfunction()

# parse_filelist_source(filelist output_var)
# A helper function that parses the list of sources (usually found in "dist/filelist"). This returning a list of
# sources that can then be used to generate the necessary build rules for the wiredtiger library. Additionally
# uses the config values "WT_ARCH" and "WT_OS" when extracting platform specific sources from the file list.
#   filelist - Destination of 'filelist' file.
#   output_var - name of the output variable that will be set with the parsed sources. Output variable is set in
#       the callers scope.
function(parse_filelist_source filelist output_var)
    set(arch_host "")
    set(plat_host "")
    # Determine architecture host for our filelist parse.
    if(WT_X86)
        set(arch_host "X86_HOST")
    elseif(WT_AARCH64)
        set(arch_host "ARM64_HOST")
    elseif(WT_PPC64)
        set(arch_host "POWERPC_HOST")
    elseif(WT_S390X)
        set(arch_host "ZSERIES_HOST")
    elseif(WT_RISCV64)
        set(arch_host "RISCV64_HOST")
    endif()
    # Determine platform host for our filelist parse.
    if(WT_POSIX)
        set(plat_host "POSIX_HOST")
    elseif(WT_WIN)
        set(plat_host "WINDOWS_HOST")
    endif()

    # Read file list and parse into list.
    file(READ "${filelist}" contents NEWLINE_CONSUME)
    string(REGEX REPLACE "\n" ";" contents "${contents}")
    set(output_files "")
    foreach(file ${contents})
        if(${file} MATCHES "^#.*$")
            continue()
        endif()
        string(REGEX REPLACE "[ \t\r]+" ";" file_contents ${file})
        list(LENGTH file_contents file_contents_len)
        if (file_contents_len EQUAL 1)
            list(APPEND output_files ${file})
        elseif(file_contents_len EQUAL 2)
            list(GET file_contents 0 file_name)
            list(GET file_contents 1 file_group)
            if ((${file_group} STREQUAL "${plat_host}") OR (${file_group} STREQUAL "${arch_host}"))
                list(APPEND output_files ${file_name})
                get_filename_component(file_ext ${file_name} EXT)
                # POWERPC and ZSERIES hosts use the '.sx' extension for their ASM files. We need to
                # manually tell CMake to ASM compile these files otherwise it will ignore them during
                # compilation process.
                if("${file_ext}" STREQUAL ".sx")
                    if("${CMAKE_C_COMPILER_ID}" MATCHES "[Cc]lang")
                        # If compiling PPC and ZSERIES assembly with Clang, we need to explicitly pass the language
                        # type onto the compiler, since the 'sx' extension is unknown.
                        set_source_files_properties(${file_name} PROPERTIES COMPILE_FLAGS "-x assembler-with-cpp")
                    endif()
                    set_source_files_properties(${file_name} PROPERTIES LANGUAGE ASM)
                endif()
            endif()
        else()
            message(FATAL_ERROR "filelist (${filelist}) has an unexpected format [Invalid Line: \"${file}]\"")
        endif()
    endforeach()
    set(${output_var} ${output_files} PARENT_SCOPE)
endfunction()

macro(source_python3_package python_libs python_version python_executable)
    set(required_version)
    if(PYTHON3_REQUIRED_VERSION)
        if(NOT PYTHON3_REQUIRED_VERSION MATCHES "^([0-9]+)(\\.[0-9]+(\\.[0-9]+)?)?$")
            message(FATAL_ERROR "Invalid value for PYTHON3_REQUIRED_VERSION: Requires a valid version string \
                Provide a version number following the format: major[.minor[.patch]]")
        endif()
        if ("${PYTHON3_REQUIRED_VERSION}" VERSION_LESS 3)
            message(FATAL_ERROR "Invalid value for PYTHON3_REQUIRED_VERSION: Requires a Python version >= 3")
        endif()
        set(required_version ${PYTHON3_REQUIRED_VERSION} EXACT)
    endif()
    if("${CMAKE_VERSION}" VERSION_LESS "3.12.0")
        # This method of finding python libs has been deprecated since version 3.12.
        # If we are running with a greater CMake version, opt to use the Python3 package.
        set(Python_ADDITIONAL_VERSIONS 3.9 3.8 3.7 3.6 3.5)
        find_package(PythonInterp ${required_version} REQUIRED)
        find_package(PythonLibs ${required_version} REQUIRED)
        include_directories(${PYTHON_INCLUDE_DIRS})
        set(${python_libs} ${PYTHON_LIBRARIES})
        set(${python_version} ${PYTHON_VERSION_STRING})
        set(${python_executable} ${PYTHON_EXECUTABLE})
    else()
        find_package(Python3 ${required_version} COMPONENTS Interpreter Development REQUIRED)
        include_directories(${Python3_INCLUDE_DIRS})
        set(${python_libs} ${Python3_LIBRARIES})
        set(${python_version} ${Python3_VERSION})
        set(${python_executable} ${Python3_EXECUTABLE})
    endif()

endmacro()
