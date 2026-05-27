# Helper function for evaluating a list of dependencies. Mostly used by the
# "config_X" helpers to evaluate the dependencies required to enable the config
# option.
#   depends - a list (semicolon separated) of dependencies to evaluate.
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
#   DEPENDS <deps> - list of dependencies (semicolon separated) required for the configuration string
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
#       separated list consisting of "<option-name>;<config-name>;<option-dependencies>".
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
#   DEPENDS <deps> - list of dependencies (semicolon separated) required for the configuration bool
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
            set(${config_name} ${CONFIG_BOOL_DEFAULT} CACHE BOOL "${description}" FORCE)
        else()
            set(${config_name} ${CONFIG_BOOL_DEFAULT} CACHE BOOL "${description}")
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
        set(${config_name} OFF CACHE BOOL "${description}" FORCE)
        set(${config_name}_DISABLED ON CACHE INTERNAL "" FORCE)
    endif()
endfunction()

# wt_find_library(NAME <name>
#                 [CMAKE_TARGET <target>]
#                 [PACKAGE <pkg> TARGET <target>]
#                 [PKGCONFIG_MODULE <mod>]
#                 [LIBRARY <libname>]
#                 [HEADER <hdr>])
#
# Discover a third-party library through CMake's canonical lookup chain:
#
#   1. find_package(<PACKAGE> QUIET)
#        Tries MODULE mode (CMake-shipped Find<Pkg>.cmake) then CONFIG mode
#        (library-shipped <Pkg>Config.cmake).
#   2. pkg_check_modules(... IMPORTED_TARGET <PKGCONFIG_MODULE>)
#        Falls back to pkg-config metadata.
#   3. find_library(<LIBRARY>) + find_path(<HEADER>)
#        Raw filesystem search; constructs an UNKNOWN IMPORTED target.
#
# Each step is attempted only if the relevant arguments are provided. The first
# successful step wins; the rest are skipped.
#
# On success:
#   HAVE_LIB${upper(NAME)} cache variable set ON.
#   wt::${CMAKE_TARGET or NAME} alias created from the discovered imported target.
# On failure:
#   HAVE_LIB${upper(NAME)} cache variable set OFF.
#
# Examples:
#   wt_find_library(NAME lz4
#       PACKAGE lz4 TARGET LZ4::lz4
#       PKGCONFIG_MODULE liblz4
#       HEADER lz4.h)
#
#   wt_find_library(NAME z CMAKE_TARGET zlib
#       PACKAGE ZLIB TARGET ZLIB::ZLIB
#       PKGCONFIG_MODULE zlib
#       HEADER zlib.h)
function(wt_find_library)
    cmake_parse_arguments(
        PARSE_ARGV
        0
        "WTLIB"
        ""
        "NAME;CMAKE_TARGET;PACKAGE;TARGET;PKGCONFIG_MODULE;LIBRARY;HEADER"
        ""
    )

    if(NOT WTLIB_NAME)
        message(FATAL_ERROR "wt_find_library: NAME is required")
    endif()
    if(WTLIB_PACKAGE AND NOT WTLIB_TARGET)
        message(FATAL_ERROR "wt_find_library(${WTLIB_NAME}): PACKAGE requires TARGET")
    endif()

    string(TOUPPER "${WTLIB_NAME}" _name_upper)
    set(_have_var "HAVE_LIB${_name_upper}")

    if(WTLIB_CMAKE_TARGET)
        set(_alias "wt::${WTLIB_CMAKE_TARGET}")
    else()
        set(_alias "wt::${WTLIB_NAME}")
    endif()

    # Guard against repeated work.
    if(TARGET ${_alias})
        return()
    endif()

    if(WTLIB_LIBRARY)
        set(_libname "${WTLIB_LIBRARY}")
    else()
        set(_libname "${WTLIB_NAME}")
    endif()

    message(CHECK_START "Looking for library ${_libname}")

    set(_imported "")

    # Step 1: find_package (MODULE then CONFIG by default).
    if(WTLIB_PACKAGE)
        find_package(${WTLIB_PACKAGE} QUIET)
        if(${WTLIB_PACKAGE}_FOUND AND TARGET ${WTLIB_TARGET})
            set(_imported ${WTLIB_TARGET})
        endif()
    endif()

    # Step 2: pkg-config.
    if(NOT _imported AND WTLIB_PKGCONFIG_MODULE)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(${_name_upper} QUIET IMPORTED_TARGET ${WTLIB_PKGCONFIG_MODULE})
            if(${_name_upper}_FOUND)
                set(_imported "PkgConfig::${_name_upper}")
            endif()
        endif()
    endif()

    # Step 3: raw find_library + find_path.
    if(NOT _imported)
        find_library(${_name_upper}_LIBRARY ${_libname})
        if(WTLIB_HEADER)
            find_path(${_name_upper}_INCLUDE_DIR ${WTLIB_HEADER})
        endif()
        if(${_name_upper}_LIBRARY AND (NOT WTLIB_HEADER OR ${_name_upper}_INCLUDE_DIR))
            set(_raw "wt_imported_${WTLIB_NAME}")
            if(NOT TARGET ${_raw})
                add_library(${_raw} UNKNOWN IMPORTED GLOBAL)
                set_target_properties(${_raw} PROPERTIES
                    IMPORTED_LOCATION "${${_name_upper}_LIBRARY}")
                if(WTLIB_HEADER)
                    set_target_properties(${_raw} PROPERTIES
                        INTERFACE_INCLUDE_DIRECTORIES "${${_name_upper}_INCLUDE_DIR}")
                endif()
            endif()
            set(_imported ${_raw})
        endif()
    endif()

    if(_imported)
        set(${_have_var} ON CACHE INTERNAL "${WTLIB_NAME} available on system")
        if(NOT TARGET ${_alias})
            add_library(${_alias} ALIAS ${_imported})
        endif()
        message(CHECK_PASS "found")
    else()
        set(${_have_var} OFF CACHE INTERNAL "${WTLIB_NAME} available on system")
        message(CHECK_FAIL "not found")
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
    elseif(WT_LOONGARCH64)
        set(arch_host "LOONGARCH64_HOST")
    endif()

    if(WT_LINUX)
        set(plat_host "LINUX_HOST;POSIX_HOST")
    elseif(WT_DARWIN)
        set(plat_host "DARWIN_HOST;POSIX_HOST")
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

            # CMake does not support testing for membership in a list.
            set(plat_index "-1")
            list(FIND plat_host "${file_group}" plat_index)
            if (("${plat_index}" GREATER_EQUAL "0") OR (${file_group} STREQUAL "${arch_host}"))
                list(APPEND output_files ${file_name})
            endif()
        else()
            message(FATAL_ERROR "filelist (${filelist}) has an unexpected format [Invalid Line: \"${file}]\"")
        endif()
    endforeach()
    set(${output_var} ${output_files} PARENT_SCOPE)
endfunction()

# add_cmake_flag(dest_var flag)
# A helper function that adds a CMake flag to a list of included flags if it's not already present.
function(add_cmake_flag included_flags flag)
    # Add extra spaces to ensure we only match whole flags.
    # This avoids partial matches and ensures correct match at the start/end of the string.
    string(FIND " ${${included_flags}} " " ${flag} " flag_position)
    if(flag_position EQUAL -1)
        set(${included_flags} "${${included_flags}} ${flag}" CACHE STRING "" FORCE)
    endif()
endfunction()

# replace_compile_options(flag_var [REMOVE <flags...>] [ADD <flags...>])
# A helper function that removes specified compiler flags from a flag variable and optionally adds new ones.
# This is useful for replacing default compiler flags with custom ones while maintaining clean flag strings.
#   flag_var - name of the variable containing compiler flags to modify (modified in parent scope)
#   REMOVE <flags...> - list of flags to remove from the flag variable
#   ADD <flags...> - list of flags to add to the flag variable after removal
function(replace_compile_options flag_var)
    cmake_parse_arguments(
        PARSE_ARGV
        1
        "REPLACE"
        ""
        ""
        "REMOVE;ADD"
    )

    # Remove existing flags
    foreach(flag ${REPLACE_REMOVE})
        # Add extra spaces to ensure we only match whole flags.
        # This avoids partial matches and ensures correct match at the start/end of the string.
        string(REPLACE " ${flag} " "" ${flag_var} " ${${flag_var}} ")
    endforeach()

    # Clean up extra spaces
    string(STRIP "${${flag_var}}" ${flag_var})

    # Add custom flags if provided
    foreach(flag ${REPLACE_ADD})
        set(${flag_var} "${${flag_var}} ${flag}")
    endforeach()

    # Clean up extra spaces
    string(STRIP "${${flag_var}}" ${flag_var})

    set(${flag_var} "${${flag_var}}" CACHE STRING "" FORCE)
endfunction()
