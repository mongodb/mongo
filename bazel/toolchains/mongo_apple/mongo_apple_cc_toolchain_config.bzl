# Copyright 2019 The Bazel Authors. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""A C++ toolchain configuration rule for macOS."""

load("@bazel_features//:features.bzl", "bazel_features")
load("@bazel_tools//tools/build_defs/cc:action_names.bzl", "ACTION_NAMES")
load(
    "@bazel_tools//tools/cpp:cc_toolchain_config_lib.bzl",
    "action_config",
    "artifact_name_pattern",
    "env_entry",
    "env_set",
    "feature",
    "feature_set",
    "flag_group",
    "flag_set",
    "make_variable",
    "tool",
    "tool_path",
    "variable_with_value",
    "with_feature_set",
)
load("@build_bazel_apple_support//lib:apple_support.bzl", "apple_support")

###
# mongodb customization
load("@//bazel/toolchains/mongo_apple:mongo_custom_features.bzl", "mongo_frameworks_feature")
###

# TODO: Remove when we drop bazel 6.x support
_OBJC_ARCHIVE_ACTION_NAME = "objc-archive"
_OBJCPP_EXECUTABLE_ACTION_NAME = "objc++-executable"

_DYNAMIC_LINK_ACTIONS = [
    ACTION_NAMES.cpp_link_dynamic_library,
    ACTION_NAMES.cpp_link_executable,
    ACTION_NAMES.cpp_link_nodeps_dynamic_library,
    ACTION_NAMES.objc_executable,
    _OBJCPP_EXECUTABLE_ACTION_NAME,
]

all_c_compile_actions = [
    ACTION_NAMES.c_compile,
    ACTION_NAMES.assemble,
    ACTION_NAMES.preprocess_assemble,
]

def _sdk_version_for_platform(xcode_config, platform_type):
    if platform_type == apple_common.platform_type.ios:
        return xcode_config.sdk_version_for_platform(apple_common.platform.ios_device)
    elif platform_type == apple_common.platform_type.tvos:
        return xcode_config.sdk_version_for_platform(apple_common.platform.tvos_device)
    elif platform_type == getattr(apple_common.platform_type, "visionos", None):
        return xcode_config.sdk_version_for_platform(apple_common.platform.visionos_device)
    elif platform_type == apple_common.platform_type.watchos:
        return xcode_config.sdk_version_for_platform(apple_common.platform.watchos_device)
    elif platform_type == apple_common.platform_type.macos:
        return xcode_config.sdk_version_for_platform(apple_common.platform.macos)
    else:
        fail("Unhandled platform type: {}".format(platform_type))

def _sdk_name(platform_type, is_simulator):
    if platform_type == apple_common.platform_type.ios and is_simulator:
        return "iPhoneSimulator"
    elif platform_type == apple_common.platform_type.ios:
        return "iPhoneOS"
    elif platform_type == getattr(apple_common.platform_type, "visionos", None) and is_simulator:
        return "XRSimulator"
    elif platform_type == getattr(apple_common.platform_type, "visionos", None):
        return "XROS"
    elif platform_type == apple_common.platform_type.watchos and is_simulator:
        return "WatchSimulator"
    elif platform_type == apple_common.platform_type.watchos:
        return "WatchOS"
    elif platform_type == apple_common.platform_type.tvos and is_simulator:
        return "AppleTVSimulator"
    elif platform_type == apple_common.platform_type.tvos:
        return "AppleTVOS"
    elif platform_type == apple_common.platform_type.macos:
        return "MacOSX"
    else:
        fail("Unhandled platform type: {}".format(platform_type))

def _impl(ctx):
    if ctx.attr.cpu.startswith("darwin"):
        platform_type = apple_common.platform_type.macos
    elif ctx.attr.cpu.startswith("ios"):
        platform_type = apple_common.platform_type.ios
    elif ctx.attr.cpu.startswith("tvos"):
        platform_type = apple_common.platform_type.tvos
    elif ctx.attr.cpu.startswith("watchos"):
        platform_type = apple_common.platform_type.watchos
    elif ctx.attr.cpu.startswith("visionos"):
        # TODO: Remove when we drop bazel 5.x support, falling back to iOS
        # doesn't hurt since you can't build for visionOS in this case anyways
        platform_type = getattr(apple_common.platform_type, "visionos", None) or apple_common.platform_type.ios
    else:
        fail("""\
Unknown CPU: {cpu}. Please update 'apple_support' to the latest version. If \
you are sure you are on the latest version, try 'bazel shutdown' to work \
around a Bazel staleness bug. Finally, if you still encounter this error, \
please file an issue at https://github.com/bazelbuild/apple_support/issues/new
""".format(cpu = ctx.attr.cpu))

    xcode_config = ctx.attr._xcode_config[apple_common.XcodeVersionConfig]
    xcode_execution_requirements = xcode_config.execution_info().keys()
    target_os_version = xcode_config.minimum_os_for_platform_type(platform_type)
    sdk_version = _sdk_version_for_platform(xcode_config, platform_type)

    is_simulator = False
    if (ctx.attr.cpu == "ios_arm64"):
        target_system_name = "arm64-apple-ios{}".format(target_os_version)
    elif (ctx.attr.cpu == "tvos_arm64"):
        target_system_name = "arm64-apple-tvos{}".format(target_os_version)
    elif (ctx.attr.cpu == "visionos_arm64"):
        target_system_name = "arm64-apple-xros{}".format(target_os_version)
    elif (ctx.attr.cpu == "watchos_arm64_32"):
        target_system_name = "arm64_32-apple-watchos{}".format(target_os_version)
    elif (ctx.attr.cpu == "ios_arm64e"):
        target_system_name = "arm64e-apple-ios{}".format(target_os_version)
    elif (ctx.attr.cpu == "watchos_armv7k"):
        target_system_name = "armv7k-apple-watchos{}".format(target_os_version)
    elif (ctx.attr.cpu == "ios_x86_64"):
        target_system_name = "x86_64-apple-ios{}-simulator".format(target_os_version)
        is_simulator = True
    elif (ctx.attr.cpu == "ios_sim_arm64"):
        target_system_name = "arm64-apple-ios{}-simulator".format(target_os_version)
        is_simulator = True
    elif (ctx.attr.cpu == "tvos_sim_arm64"):
        target_system_name = "arm64-apple-tvos{}-simulator".format(target_os_version)
        is_simulator = True
    elif (ctx.attr.cpu == "visionos_sim_arm64"):
        target_system_name = "arm64-apple-xros{}-simulator".format(target_os_version)
        is_simulator = True
    elif (ctx.attr.cpu == "watchos_arm64"):
        target_system_name = "arm64-apple-watchos{}-simulator".format(target_os_version)
        is_simulator = True
    elif (ctx.attr.cpu == "watchos_device_arm64"):
        target_system_name = "arm64-apple-watchos{}".format(target_os_version)
    elif (ctx.attr.cpu == "watchos_device_arm64e"):
        target_system_name = "arm64e-apple-watchos{}".format(target_os_version)
    elif (ctx.attr.cpu == "darwin_x86_64"):
        target_system_name = "x86_64-apple-macosx{}".format(target_os_version)
    elif (ctx.attr.cpu == "darwin_arm64"):
        target_system_name = "arm64-apple-macosx{}".format(target_os_version)
    elif (ctx.attr.cpu == "darwin_arm64e"):
        target_system_name = "arm64e-apple-macosx{}".format(target_os_version)
    elif (ctx.attr.cpu == "tvos_x86_64"):
        target_system_name = "x86_64-apple-tvos{}-simulator".format(target_os_version)
        is_simulator = True
    elif (ctx.attr.cpu == "watchos_x86_64"):
        target_system_name = "x86_64-apple-watchos{}-simulator".format(target_os_version)
        is_simulator = True
    else:
        fail("""\
Unknown CPU: {cpu}. Please update 'apple_support' to the latest version. If \
you are sure you are on the latest version, try 'bazel shutdown' to work \
around a Bazel staleness bug. Finally, if you still encounter this error, \
please file an issue at https://github.com/bazelbuild/apple_support/issues/new
""".format(cpu = ctx.attr.cpu))

    if ctx.attr.cpu.startswith("darwin_"):
        target_libc = "macosx"
    else:
        target_libc = ctx.attr.cpu.split("_")[0]

    if ctx.attr.cpu == "darwin_x86_64":
        abi_libc_version = "darwin_x86_64"
        abi_version = "darwin_x86_64"
    else:
        abi_libc_version = "local"
        abi_version = "local"

    arch = ctx.attr.cpu.split("_", 1)[-1]
    if ctx.attr.cpu in ["ios_sim_arm64", "tvos_sim_arm64", "visionos_sim_arm64", "watchos_arm64", "watchos_device_arm64"]:
        arch = "arm64"
    elif ctx.attr.cpu in ["watchos_device_arm64e"]:
        arch = "arm64e"

    all_link_actions = [
        ACTION_NAMES.cpp_link_executable,
        ACTION_NAMES.cpp_link_dynamic_library,
        ACTION_NAMES.cpp_link_nodeps_dynamic_library,
    ]

    strip_action = action_config(
        action_name = ACTION_NAMES.strip,
        flag_sets = [
            flag_set(
                flag_groups = [
                    flag_group(flags = ["-S", "-o", "%{output_file}"]),
                    flag_group(
                        flags = ["%{stripopts}"],
                        iterate_over = "stripopts",
                    ),
                    flag_group(flags = ["%{input_file}"]),
                ],
            ),
        ],
        tools = [tool(path = "/usr/bin/strip")],
    )

    header_parsing_env_feature = feature(
        name = "header_parsing_env",
        env_sets = [
            env_set(
                actions = [ACTION_NAMES.cpp_header_parsing],
                env_entries = [
                    env_entry(
                        key = "HEADER_PARSING_OUTPUT",
                        value = "%{output_file}",
                    ),
                ],
            ),
        ],
    )

    cpp_header_parsing_action = action_config(
        action_name = ACTION_NAMES.cpp_header_parsing,
        implies = [
            "preprocessor_defines",
            "include_system_dirs",
            "objc_arc",
            "no_objc_arc",
            "apple_env",
            "user_compile_flags",
            "sysroot",
            "unfiltered_compile_flags",
            "compiler_input_flags",
            "compiler_output_flags",
            "unfiltered_cxx_flags",
            "header_parsing_env",
        ],
        flag_sets = [
            flag_set(
                flag_groups = [
                    flag_group(
                        flags = [
                            # Note: This treats all headers as C++ headers, which may lead to
                            # parsing failures for C headers that are not valid C++.
                            # For such headers, use features = ["-parse_headers"] to selectively
                            # disable parsing.
                            "-xc++-header",
                            "-fsyntax-only",
                        ],
                    ),
                ],
            ),
        ],
        tools = [
            tool(
                path = "wrapped_clang",
                execution_requirements = xcode_execution_requirements,
            ),
        ],
    )

    objc_compile_action = action_config(
        action_name = ACTION_NAMES.objc_compile,
        enabled = True,
        flag_sets = [
            flag_set(
                flag_groups = [flag_group(flags = ["-target", target_system_name])],
            ),
        ],
        implies = [
            "compiler_input_flags",
            "compiler_output_flags",
            "objc_actions",
            "apply_default_compiler_flags",
            "apply_default_warnings",
            "framework_paths",
            "preprocessor_defines",
            "include_system_dirs",
            "objc_arc",
            "no_objc_arc",
            "apple_env",
            "user_compile_flags",
            "sysroot",
            "unfiltered_compile_flags",
            "apply_simulator_compiler_flags",
        ],
        tools = [
            tool(
                path = "wrapped_clang",
                execution_requirements = xcode_execution_requirements,
            ),
        ],
    )

    objc_link_flag_feature = feature(
        name = "objc_link_flag",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = [ACTION_NAMES.objc_executable, _OBJCPP_EXECUTABLE_ACTION_NAME],
                flag_groups = [flag_group(flags = ["-ObjC"])],
                with_features = [with_feature_set(not_features = ["kernel_extension"])],
            ),
        ],
    )

    objcpp_executable_action = action_config(
        action_name = _OBJCPP_EXECUTABLE_ACTION_NAME,
        flag_sets = [
            flag_set(
                flag_groups = [
                    flag_group(
                        flags = [
                            "-Xlinker",
                            "-objc_abi_version",
                            "-Xlinker",
                            "2",
                        ],
                    ),
                ],
                with_features = [with_feature_set(not_features = ["kernel_extension"])],
            ),
            flag_set(
                flag_groups = [
                    flag_group(flags = ["-target", target_system_name]),
                    flag_group(
                        flags = ["-l%{library_names}"],
                        iterate_over = "library_names",
                    ),
                    flag_group(flags = ["-filelist", "%{filelist}"]),
                    flag_group(flags = ["-o", "%{linked_binary}"]),
                    flag_group(
                        flags = ["-force_load", "%{force_load_exec_paths}"],
                        iterate_over = "force_load_exec_paths",
                    ),
                    flag_group(
                        flags = ["%{dep_linkopts}"],
                        iterate_over = "dep_linkopts",
                    ),
                    flag_group(
                        flags = ["-Wl,%{attr_linkopts}"],
                        iterate_over = "attr_linkopts",
                    ),
                ],
            ),
        ],
        implies = [
            "include_system_dirs",
            "framework_paths",
            "strip_debug_symbols",
            "apple_env",
        ],
        tools = [
            tool(
                path = "wrapped_clang",
                execution_requirements = xcode_execution_requirements,
            ),
        ],
    )

    cpp_link_dynamic_library_action = action_config(
        action_name = ACTION_NAMES.cpp_link_dynamic_library,
        implies = [
            "has_configured_linker_path",
            "shared_flag",
            "linkstamps",
            "output_execpath_flags",
            "runtime_root_flags",
            "input_param_flags",
            "strip_debug_symbols",
            "linker_param_file",
            "apple_env",
            "sysroot",
        ],
        tools = [
            tool(
                path = "cc_wrapper.sh",
                execution_requirements = xcode_execution_requirements,
            ),
        ],
    )

    cpp_link_static_library_action = action_config(
        action_name = ACTION_NAMES.cpp_link_static_library,
        implies = [
            "runtime_root_flags",
            "archiver_flags",
            "input_param_flags",
            "linker_param_file",
            "apple_env",
        ],
        tools = [
            tool(
                path = "libtool",
                execution_requirements = xcode_execution_requirements,
            ),
        ],
    )

    c_compile_action = action_config(
        action_name = ACTION_NAMES.c_compile,
        implies = [
            "preprocessor_defines",
            "include_system_dirs",
            "objc_arc",
            "no_objc_arc",
            "apple_env",
            "user_compile_flags",
            "sysroot",
            "unfiltered_compile_flags",
            "compiler_input_flags",
            "compiler_output_flags",
            "unfiltered_cxx_flags",
        ],
        tools = [
            tool(
                path = "wrapped_clang",
                execution_requirements = xcode_execution_requirements,
            ),
        ],
    )

    cpp_compile_action = action_config(
        action_name = ACTION_NAMES.cpp_compile,
        implies = [
            "preprocessor_defines",
            "include_system_dirs",
            "objc_arc",
            "no_objc_arc",
            "apple_env",
            "user_compile_flags",
            "sysroot",
            "unfiltered_compile_flags",
            "compiler_input_flags",
            "compiler_output_flags",
            "unfiltered_cxx_flags",
        ],
        tools = [
            tool(
                path = "wrapped_clang_pp",
                execution_requirements = xcode_execution_requirements,
            ),
        ],
    )

    objcpp_compile_action = action_config(
        action_name = ACTION_NAMES.objcpp_compile,
        flag_sets = [
            flag_set(
                flag_groups = [
                    flag_group(
                        flags = [
                            "-target",
                            target_system_name,
                            "-stdlib=libc++",
                            "-std=gnu++14",
                        ],
                    ),
                ],
            ),
        ],
        implies = [
            "compiler_input_flags",
            "compiler_output_flags",
            "apply_default_compiler_flags",
            "apply_default_warnings",
            "framework_paths",
            "preprocessor_defines",
            "include_system_dirs",
            "objc_arc",
            "no_objc_arc",
            "apple_env",
            "user_compile_flags",
            "sysroot",
            "unfiltered_compile_flags",
            "apply_simulator_compiler_flags",
        ],
        tools = [
            tool(
                path = "wrapped_clang_pp",
                execution_requirements = xcode_execution_requirements,
            ),
        ],
    )

    assemble_action = action_config(
        action_name = ACTION_NAMES.assemble,
        implies = [
            "objc_arc",
            "no_objc_arc",
            "include_system_dirs",
            "apple_env",
            "user_compile_flags",
            "sysroot",
            "unfiltered_compile_flags",
            "compiler_input_flags",
            "compiler_output_flags",
            "unfiltered_cxx_flags",
        ],
        tools = [
            tool(
                path = "wrapped_clang",
                execution_requirements = xcode_execution_requirements,
            ),
        ],
    )

    preprocess_assemble_action = action_config(
        action_name = ACTION_NAMES.preprocess_assemble,
        implies = [
            "preprocessor_defines",
            "include_system_dirs",
            "objc_arc",
            "no_objc_arc",
            "apple_env",
            "user_compile_flags",
            "sysroot",
            "unfiltered_compile_flags",
            "compiler_input_flags",
            "compiler_output_flags",
            "unfiltered_cxx_flags",
        ],
        tools = [
            tool(
                path = "wrapped_clang",
                execution_requirements = xcode_execution_requirements,
            ),
        ],
    )

    objc_archive_action = action_config(
        action_name = _OBJC_ARCHIVE_ACTION_NAME,
        flag_sets = [
            flag_set(
                flag_groups = [
                    flag_group(
                        flags = [
                            "-D",
                            "-no_warning_for_no_symbols",
                            "-static",
                            "-filelist",
                            "%{obj_list_path}",
                            "-arch_only",
                            arch,
                            "-syslibroot",
                            "__BAZEL_XCODE_SDKROOT__",
                            "-o",
                            "%{output_execpath}",
                        ],
                    ),
                ],
            ),
        ],
        implies = ["apple_env"],
        tools = [
            tool(
                path = "libtool",
                execution_requirements = xcode_execution_requirements,
            ),
        ],
    )

    objc_executable_action = action_config(
        action_name = "objc-executable",
        flag_sets = [
            flag_set(
                flag_groups = [
                    flag_group(
                        flags = [
                            "-Xlinker",
                            "-objc_abi_version",
                            "-Xlinker",
                            "2",
                        ],
                    ),
                ],
                with_features = [with_feature_set(not_features = ["kernel_extension"])],
            ),
            flag_set(
                flag_groups = [
                    flag_group(flags = ["-target", target_system_name]),
                    flag_group(
                        flags = ["-l%{library_names}"],
                        iterate_over = "library_names",
                    ),
                    flag_group(flags = ["-filelist", "%{filelist}"]),
                    flag_group(flags = ["-o", "%{linked_binary}"]),
                    flag_group(
                        flags = ["-force_load", "%{force_load_exec_paths}"],
                        iterate_over = "force_load_exec_paths",
                    ),
                    flag_group(
                        flags = ["%{dep_linkopts}"],
                        iterate_over = "dep_linkopts",
                    ),
                    flag_group(
                        flags = ["-Wl,%{attr_linkopts}"],
                        iterate_over = "attr_linkopts",
                    ),
                ],
            ),
        ],
        implies = [
            "include_system_dirs",
            "framework_paths",
            "strip_debug_symbols",
            "apple_env",
        ],
        tools = [
            tool(
                path = "wrapped_clang",
                execution_requirements = xcode_execution_requirements,
            ),
        ],
    )

    cpp_link_executable_action = action_config(
        action_name = ACTION_NAMES.cpp_link_executable,
        implies = [
            "linkstamps",
            "output_execpath_flags",
            "runtime_root_flags",
            "input_param_flags",
            "force_pic_flags",
            "strip_debug_symbols",
            "linker_param_file",
            "apple_env",
            "sysroot",
        ],
        tools = [
            tool(
                path = "cc_wrapper.sh",
                execution_requirements = xcode_execution_requirements,
            ),
        ],
    )

    linkstamp_compile_action = action_config(
        action_name = ACTION_NAMES.linkstamp_compile,
        implies = [
            "preprocessor_defines",
            "include_system_dirs",
            "objc_arc",
            "no_objc_arc",
            "apple_env",
            "user_compile_flags",
            "sysroot",
            "unfiltered_compile_flags",
            "compiler_input_flags",
            "compiler_output_flags",
        ],
        tools = [
            tool(
                path = "wrapped_clang",
                execution_requirements = xcode_execution_requirements,
            ),
        ],
    )

    cpp_module_compile_action = action_config(
        action_name = ACTION_NAMES.cpp_module_compile,
        implies = [
            "preprocessor_defines",
            "include_system_dirs",
            "objc_arc",
            "no_objc_arc",
            "apple_env",
            "user_compile_flags",
            "sysroot",
            "unfiltered_compile_flags",
            "compiler_input_flags",
            "compiler_output_flags",
            "unfiltered_cxx_flags",
        ],
        tools = [
            tool(
                path = "wrapped_clang",
                execution_requirements = xcode_execution_requirements,
            ),
        ],
    )

    cpp_link_nodeps_dynamic_library_action = action_config(
        action_name = ACTION_NAMES.cpp_link_nodeps_dynamic_library,
        implies = [
            "has_configured_linker_path",
            "shared_flag",
            "linkstamps",
            "output_execpath_flags",
            "runtime_root_flags",
            "input_param_flags",
            "strip_debug_symbols",
            "linker_param_file",
            "apple_env",
            "sysroot",
        ],
        tools = [
            tool(
                path = "cc_wrapper.sh",
                execution_requirements = xcode_execution_requirements,
            ),
        ],
    )

    objc_fully_link_action = action_config(
        action_name = "objc-fully-link",
        flag_sets = [
            flag_set(
                flag_groups = [
                    flag_group(
                        flags = [
                            "-D",
                            "-no_warning_for_no_symbols",
                            "-static",
                            "-arch_only",
                            arch,
                            "-syslibroot",
                            "__BAZEL_XCODE_SDKROOT__",
                            "-o",
                            "%{fully_linked_archive_path}",
                        ],
                    ),
                    flag_group(
                        flags = ["%{objc_library_exec_paths}"],
                        iterate_over = "objc_library_exec_paths",
                    ),
                    flag_group(
                        flags = ["%{cc_library_exec_paths}"],
                        iterate_over = "cc_library_exec_paths",
                    ),
                    flag_group(
                        flags = ["%{imported_library_exec_paths}"],
                        iterate_over = "imported_library_exec_paths",
                    ),
                ],
            ),
        ],
        implies = ["apple_env"],
        tools = [
            tool(
                path = "libtool",
                execution_requirements = xcode_execution_requirements,
            ),
        ],
    )

    objcopy_embed_data_action = action_config(
        action_name = "objcopy_embed_data",
        enabled = True,
        tools = [tool(path = "/usr/bin/objcopy")],
    )

    action_configs = [
        strip_action,
        c_compile_action,
        cpp_compile_action,
        linkstamp_compile_action,
        cpp_module_compile_action,
        cpp_header_parsing_action,
        objc_compile_action,
        objcpp_compile_action,
        assemble_action,
        preprocess_assemble_action,
        objc_archive_action,
        objc_executable_action,
        objcpp_executable_action,
        cpp_link_executable_action,
        cpp_link_dynamic_library_action,
        cpp_link_nodeps_dynamic_library_action,
        cpp_link_static_library_action,
        objc_fully_link_action,
        objcopy_embed_data_action,
    ]

    if (ctx.attr.cpu == "ios_arm64" or
        ctx.attr.cpu == "ios_arm64e" or
        ctx.attr.cpu == "ios_sim_arm64" or
        ctx.attr.cpu == "ios_x86_64" or
        ctx.attr.cpu == "watchos_arm64_32" or
        ctx.attr.cpu == "watchos_device_arm64" or
        ctx.attr.cpu == "watchos_device_arm64e" or
        ctx.attr.cpu == "watchos_armv7k" or
        ctx.attr.cpu == "watchos_x86_64" or
        ctx.attr.cpu == "watchos_arm64"):
        apply_default_compiler_flags_feature = feature(
            name = "apply_default_compiler_flags",
            flag_sets = [
                flag_set(
                    actions = [ACTION_NAMES.objc_compile, ACTION_NAMES.objcpp_compile],
                    flag_groups = [flag_group(flags = ["-DOS_IOS", "-fno-autolink"])],
                ),
            ],
        )
    elif (ctx.attr.cpu == "darwin_x86_64" or
          ctx.attr.cpu == "darwin_arm64" or
          ctx.attr.cpu == "darwin_arm64e"):
        apply_default_compiler_flags_feature = feature(
            name = "apply_default_compiler_flags",
            flag_sets = [
                flag_set(
                    actions = [ACTION_NAMES.objc_compile, ACTION_NAMES.objcpp_compile],
                    flag_groups = [flag_group(flags = ["-DOS_MACOSX", "-fno-autolink"])],
                ),
            ],
        )
    elif (ctx.attr.cpu == "tvos_arm64" or
          ctx.attr.cpu == "tvos_x86_64" or
          ctx.attr.cpu == "tvos_sim_arm64"):
        apply_default_compiler_flags_feature = feature(
            name = "apply_default_compiler_flags",
            flag_sets = [
                flag_set(
                    actions = [ACTION_NAMES.objc_compile, ACTION_NAMES.objcpp_compile],
                    flag_groups = [flag_group(flags = ["-DOS_TVOS", "-fno-autolink"])],
                ),
            ],
        )
    elif (
        ctx.attr.cpu == "visionos_arm64" or
        ctx.attr.cpu == "visionos_sim_arm64"
    ):
        apply_default_compiler_flags_feature = feature(
            name = "apply_default_compiler_flags",
            flag_sets = [
                flag_set(
                    actions = [ACTION_NAMES.objc_compile, ACTION_NAMES.objcpp_compile],
                    flag_groups = [flag_group(flags = ["-fno-autolink"])],
                ),
            ],
        )
    else:
        fail("""\
Unknown CPU: {cpu}. Please update 'apple_support' to the latest version. If \
you are sure you are on the latest version, try 'bazel shutdown' to work \
around a Bazel staleness bug. Finally, if you still encounter this error, \
please file an issue at https://github.com/bazelbuild/apple_support/issues/new
""".format(cpu = ctx.attr.cpu))

    runtime_root_flags_feature = feature(
        name = "runtime_root_flags",
        flag_sets = [
            flag_set(
                actions = all_link_actions +
                          [ACTION_NAMES.cpp_link_static_library],
                flag_groups = [
                    flag_group(
                        flags = [
                            "-Xlinker",
                            "-rpath",
                            "-Xlinker",
                            "@loader_path/%{runtime_library_search_directories}",
                        ],
                        iterate_over = "runtime_library_search_directories",
                        expand_if_available = "runtime_library_search_directories",
                    ),
                ],
            ),
        ],
    )

    objc_arc_feature = feature(
        name = "objc_arc",
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.cpp_module_compile,
                    ACTION_NAMES.cpp_header_parsing,
                    ACTION_NAMES.assemble,
                    ACTION_NAMES.preprocess_assemble,
                    ACTION_NAMES.objc_compile,
                    ACTION_NAMES.objcpp_compile,
                ],
                flag_groups = [
                    flag_group(
                        flags = ["-fobjc-arc"],
                        expand_if_available = "objc_arc",
                    ),
                ],
            ),
        ],
    )

    unfiltered_cxx_flags_feature = feature(
        name = "unfiltered_cxx_flags",
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.cpp_module_compile,
                    ACTION_NAMES.cpp_header_parsing,
                    ACTION_NAMES.assemble,
                    ACTION_NAMES.preprocess_assemble,
                ],
                flag_groups = [
                    flag_group(flags = ["-no-canonical-prefixes", "-pthread"]),
                ],
            ),
        ],
    )

    compiler_input_flags_feature = feature(
        name = "compiler_input_flags",
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.assemble,
                    ACTION_NAMES.preprocess_assemble,
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.linkstamp_compile,
                    ACTION_NAMES.cpp_header_parsing,
                    ACTION_NAMES.cpp_module_compile,
                    ACTION_NAMES.cpp_module_codegen,
                    ACTION_NAMES.objc_compile,
                    ACTION_NAMES.objcpp_compile,
                ],
                flag_groups = [
                    flag_group(
                        flags = ["-c", "%{source_file}"],
                        expand_if_available = "source_file",
                    ),
                ],
            ),
        ],
    )

    external_include_paths_feature = feature(
        name = "external_include_paths",
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.preprocess_assemble,
                    ACTION_NAMES.linkstamp_compile,
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.cpp_header_parsing,
                    ACTION_NAMES.cpp_module_compile,
                    ACTION_NAMES.clif_match,
                    ACTION_NAMES.objc_compile,
                    ACTION_NAMES.objcpp_compile,
                ],
                flag_groups = [
                    flag_group(
                        flags = ["-isystem", "%{external_include_paths}"],
                        iterate_over = "external_include_paths",
                        expand_if_available = "external_include_paths",
                    ),
                ],
            ),
        ],
    )

    strip_debug_symbols_feature = feature(
        name = "strip_debug_symbols",
        flag_sets = [
            flag_set(
                actions = _DYNAMIC_LINK_ACTIONS,
                flag_groups = [
                    flag_group(
                        flags = ["-Wl,-S"],
                        expand_if_available = "strip_debug_symbols",
                    ),
                ],
            ),
        ],
    )

    shared_flag_feature = feature(
        name = "shared_flag",
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.cpp_link_dynamic_library,
                    ACTION_NAMES.cpp_link_nodeps_dynamic_library,
                ],
                flag_groups = [flag_group(flags = ["-shared"])],
            ),
        ],
    )

    if is_simulator:
        apply_simulator_compiler_flags_feature = feature(
            name = "apply_simulator_compiler_flags",
            flag_sets = [
                flag_set(
                    actions = [ACTION_NAMES.objc_compile, ACTION_NAMES.objcpp_compile],
                    flag_groups = [
                        flag_group(
                            flags = [
                                "-fexceptions",
                                "-fasm-blocks",
                                "-fobjc-abi-version=2",
                                "-fobjc-legacy-dispatch",
                            ],
                        ),
                    ],
                ),
            ],
        )
    else:
        apply_simulator_compiler_flags_feature = feature(name = "apply_simulator_compiler_flags")

    user_link_flags_feature = feature(
        name = "user_link_flags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = _DYNAMIC_LINK_ACTIONS,
                flag_groups = [
                    flag_group(
                        flags = ["%{user_link_flags}"],
                        iterate_over = "user_link_flags",
                        expand_if_available = "user_link_flags",
                    ),
                ],
            ),
        ],
    )

    includes_feature = feature(
        name = "includes",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.preprocess_assemble,
                    ACTION_NAMES.linkstamp_compile,
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.cpp_header_parsing,
                    ACTION_NAMES.cpp_module_compile,
                    ACTION_NAMES.objc_compile,
                    ACTION_NAMES.objcpp_compile,
                    ACTION_NAMES.clif_match,
                ],
                flag_groups = [
                    flag_group(
                        flags = ["-include", "%{includes}"],
                        iterate_over = "includes",
                        expand_if_available = "includes",
                    ),
                ],
            ),
        ],
    )

    gcc_coverage_map_format_feature = feature(
        name = "gcc_coverage_map_format",
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.preprocess_assemble,
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.cpp_module_compile,
                    ACTION_NAMES.objc_compile,
                    ACTION_NAMES.objcpp_compile,
                ],
                flag_groups = [
                    flag_group(
                        flags = ["-fprofile-arcs", "-ftest-coverage", "-g"],
                    ),
                ],
            ),
            flag_set(
                actions = [
                    ACTION_NAMES.cpp_link_dynamic_library,
                    ACTION_NAMES.cpp_link_nodeps_dynamic_library,
                    ACTION_NAMES.cpp_link_executable,
                ],
                flag_groups = [flag_group(flags = ["--coverage"])],
            ),
        ],
        requires = [feature_set(features = ["coverage"])],
    )

    gcc_quoting_for_param_files_feature = feature(
        name = "gcc_quoting_for_param_files",
        enabled = bazel_features.cc.fixed_dsym_path_quoting,
    )

    default_link_flags_feature = feature(
        name = "default_link_flags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = _DYNAMIC_LINK_ACTIONS,
                flag_groups = [
                    flag_group(
                        flags = [
                            "-no-canonical-prefixes",
                            "-target",
                            target_system_name,
                            "-fobjc-link-runtime",
                        ],
                    ),
                ],
            ),
            flag_set(
                actions = _DYNAMIC_LINK_ACTIONS,
                flag_groups = [flag_group(flags = ["-dead_strip"])],
                with_features = [with_feature_set(features = ["opt"])],
            ),
        ],
    )

    no_deduplicate_feature = feature(
        name = "no_deduplicate",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = _DYNAMIC_LINK_ACTIONS,
                flag_groups = [
                    flag_group(
                        flags = [
                            "-Xlinker",
                            "-no_deduplicate",
                        ],
                    ),
                ],
                with_features = [
                    with_feature_set(not_features = ["opt"]),
                ],
            ),
        ],
    )

    output_execpath_flags_feature = feature(
        name = "output_execpath_flags",
        flag_sets = [
            flag_set(
                actions = all_link_actions,
                flag_groups = [
                    flag_group(
                        flags = ["-o", "%{output_execpath}"],
                        expand_if_available = "output_execpath",
                    ),
                ],
            ),
        ],
    )

    pic_feature = feature(
        name = "pic",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.cpp_module_codegen,
                    ACTION_NAMES.cpp_module_compile,
                    ACTION_NAMES.linkstamp_compile,
                    ACTION_NAMES.preprocess_assemble,
                ],
                flag_groups = [
                    flag_group(flags = ["-fPIC"], expand_if_available = "pic"),
                ],
            ),
        ],
    )

    framework_paths_feature = feature(
        name = "framework_paths",
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.preprocess_assemble,
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.cpp_header_parsing,
                    ACTION_NAMES.cpp_module_compile,
                    ACTION_NAMES.objc_compile,
                    ACTION_NAMES.objcpp_compile,
                ],
                flag_groups = [
                    flag_group(
                        flags = ["-F%{framework_include_paths}"],
                        iterate_over = "framework_include_paths",
                    ),
                ],
            ),
            flag_set(
                actions = [
                    "objc-executable",
                    _OBJCPP_EXECUTABLE_ACTION_NAME,
                ],
                flag_groups = [
                    flag_group(
                        flags = ["-F%{framework_paths}"],
                        iterate_over = "framework_paths",
                    ),
                    flag_group(
                        flags = ["-framework", "%{framework_names}"],
                        iterate_over = "framework_names",
                    ),
                    flag_group(
                        flags = ["-weak_framework", "%{weak_framework_names}"],
                        iterate_over = "weak_framework_names",
                    ),
                ],
            ),
        ],
    )

    compiler_output_flags_feature = feature(
        name = "compiler_output_flags",
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.assemble,
                    ACTION_NAMES.preprocess_assemble,
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.linkstamp_compile,
                    ACTION_NAMES.cpp_header_parsing,
                    ACTION_NAMES.cpp_module_compile,
                    ACTION_NAMES.cpp_module_codegen,
                    ACTION_NAMES.objc_compile,
                    ACTION_NAMES.objcpp_compile,
                ],
                flag_groups = [
                    flag_group(
                        flags = ["-S"],
                        expand_if_available = "output_assembly_file",
                    ),
                    flag_group(
                        flags = ["-E"],
                        expand_if_available = "output_preprocess_file",
                    ),
                    flag_group(
                        flags = ["-o", "%{output_file}"],
                        expand_if_available = "output_file",
                    ),
                ],
            ),
        ],
    )

    pch_feature = feature(
        name = "pch",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.objc_compile,
                    ACTION_NAMES.objcpp_compile,
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                ],
                flag_groups = [
                    flag_group(
                        flags = [
                            "-include",
                            "%{pch_file}",
                        ],
                        expand_if_available = "pch_file",
                    ),
                ],
            ),
        ],
    )

    include_system_dirs_feature = feature(
        name = "include_system_dirs",
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.cpp_module_compile,
                    ACTION_NAMES.cpp_header_parsing,
                    ACTION_NAMES.objc_compile,
                    ACTION_NAMES.objcpp_compile,
                    "objc-executable",
                    _OBJCPP_EXECUTABLE_ACTION_NAME,
                    ACTION_NAMES.assemble,
                    ACTION_NAMES.preprocess_assemble,
                ],
                flag_groups = [
                    flag_group(
                        flags = [
                            "-isysroot",
                            "__BAZEL_XCODE_SDKROOT__",
                            "-F__BAZEL_XCODE_SDKROOT__/System/Library/Frameworks",
                            "-F{}".format(apple_support.path_placeholders.platform_frameworks(apple_fragment = ctx.fragments.apple)),
                        ],
                    ),
                ],
            ),
        ],
    )

    input_param_flags_feature = feature(
        name = "input_param_flags",
        flag_sets = [
            flag_set(
                actions = all_link_actions +
                          [ACTION_NAMES.cpp_link_static_library],
                flag_groups = [
                    flag_group(
                        flags = ["-L%{library_search_directories}"],
                        iterate_over = "library_search_directories",
                        expand_if_available = "library_search_directories",
                    ),
                ],
            ),
            flag_set(
                actions = all_link_actions +
                          [ACTION_NAMES.cpp_link_static_library],
                flag_groups = [
                    flag_group(
                        flags = ["%{libopts}"],
                        iterate_over = "libopts",
                        expand_if_available = "libopts",
                    ),
                ],
            ),
            flag_set(
                actions = all_link_actions +
                          [ACTION_NAMES.cpp_link_static_library],
                flag_groups = [
                    flag_group(
                        flags = ["-Wl,-force_load,%{whole_archive_linker_params}"],
                        iterate_over = "whole_archive_linker_params",
                        expand_if_available = "whole_archive_linker_params",
                    ),
                ],
            ),
            flag_set(
                actions = all_link_actions +
                          [ACTION_NAMES.cpp_link_static_library],
                flag_groups = [
                    flag_group(
                        flags = ["%{linker_input_params}"],
                        iterate_over = "linker_input_params",
                        expand_if_available = "linker_input_params",
                    ),
                ],
            ),
            flag_set(
                actions = all_link_actions +
                          [ACTION_NAMES.cpp_link_static_library],
                flag_groups = [
                    flag_group(
                        iterate_over = "libraries_to_link",
                        flag_groups = [
                            flag_group(
                                iterate_over = "libraries_to_link.object_files",
                                flag_groups = [
                                    flag_group(
                                        flags = ["%{libraries_to_link.object_files}"],
                                        expand_if_false = "libraries_to_link.is_whole_archive",
                                    ),
                                    flag_group(
                                        flags = ["-Wl,-force_load,%{libraries_to_link.object_files}"],
                                        expand_if_true = "libraries_to_link.is_whole_archive",
                                    ),
                                ],
                                expand_if_equal = variable_with_value(
                                    name = "libraries_to_link.type",
                                    value = "object_file_group",
                                ),
                            ),
                            flag_group(
                                flag_groups = [
                                    flag_group(
                                        flags = ["%{libraries_to_link.name}"],
                                        expand_if_false = "libraries_to_link.is_whole_archive",
                                    ),
                                    flag_group(
                                        flags = ["-Wl,-force_load,%{libraries_to_link.name}"],
                                        expand_if_true = "libraries_to_link.is_whole_archive",
                                    ),
                                ],
                                expand_if_equal = variable_with_value(
                                    name = "libraries_to_link.type",
                                    value = "object_file",
                                ),
                            ),
                            flag_group(
                                flag_groups = [
                                    flag_group(
                                        flags = ["%{libraries_to_link.name}"],
                                        expand_if_false = "libraries_to_link.is_whole_archive",
                                    ),
                                    flag_group(
                                        flags = ["-Wl,-force_load,%{libraries_to_link.name}"],
                                        expand_if_true = "libraries_to_link.is_whole_archive",
                                    ),
                                ],
                                expand_if_equal = variable_with_value(
                                    name = "libraries_to_link.type",
                                    value = "interface_library",
                                ),
                            ),
                            flag_group(
                                flag_groups = [
                                    flag_group(
                                        flags = ["%{libraries_to_link.name}"],
                                        expand_if_false = "libraries_to_link.is_whole_archive",
                                    ),
                                    flag_group(
                                        flags = ["-Wl,-force_load,%{libraries_to_link.name}"],
                                        expand_if_true = "libraries_to_link.is_whole_archive",
                                    ),
                                ],
                                expand_if_equal = variable_with_value(
                                    name = "libraries_to_link.type",
                                    value = "static_library",
                                ),
                            ),
                            flag_group(
                                flag_groups = [
                                    flag_group(
                                        flags = ["-l%{libraries_to_link.name}"],
                                        expand_if_false = "libraries_to_link.is_whole_archive",
                                    ),
                                    flag_group(
                                        flags = ["-Wl,-force_load,-l%{libraries_to_link.name}"],
                                        expand_if_true = "libraries_to_link.is_whole_archive",
                                    ),
                                ],
                                expand_if_equal = variable_with_value(
                                    name = "libraries_to_link.type",
                                    value = "dynamic_library",
                                ),
                            ),
                            flag_group(
                                flag_groups = [
                                    flag_group(
                                        flags = ["%{libraries_to_link.path}"],
                                    ),
                                ],
                                expand_if_equal = variable_with_value(
                                    name = "libraries_to_link.type",
                                    value = "versioned_dynamic_library",
                                ),
                            ),
                        ],
                        expand_if_available = "libraries_to_link",
                    ),
                ],
            ),
        ],
    )

    per_object_debug_info_feature = feature(
        name = "per_object_debug_info",
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.cpp_module_codegen,
                    ACTION_NAMES.assemble,
                    ACTION_NAMES.preprocess_assemble,
                ],
                flag_groups = [
                    flag_group(
                        flags = ["-gsplit-dwarf", "-g"],
                        expand_if_available = "per_object_debug_info_file",
                    ),
                ],
            ),
        ],
    )

    lipo_feature = feature(
        name = "lipo",
        flag_sets = [
            flag_set(
                actions = [ACTION_NAMES.c_compile, ACTION_NAMES.cpp_compile],
                flag_groups = [flag_group(flags = ["-fripa"])],
            ),
        ],
        requires = [
            feature_set(features = ["autofdo"]),
            feature_set(features = ["fdo_optimize"]),
            feature_set(features = ["fdo_instrument"]),
        ],
    )

    apple_env_feature = feature(
        name = "apple_env",
        env_sets = [
            env_set(
                actions = _DYNAMIC_LINK_ACTIONS + [
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.cpp_module_compile,
                    ACTION_NAMES.cpp_header_parsing,
                    ACTION_NAMES.assemble,
                    ACTION_NAMES.preprocess_assemble,
                    ACTION_NAMES.objc_compile,
                    ACTION_NAMES.objcpp_compile,
                    _OBJC_ARCHIVE_ACTION_NAME,
                    "objc-fully-link",
                    ACTION_NAMES.cpp_link_static_library,
                    ACTION_NAMES.linkstamp_compile,
                ],
                env_entries = [
                    env_entry(
                        key = "XCODE_VERSION_OVERRIDE",
                        value = str(xcode_config.xcode_version()),
                    ),
                    # TODO: Remove once we drop bazel 7.x support
                    env_entry(
                        key = "APPLE_SDK_VERSION_OVERRIDE",
                        value = str(sdk_version),
                    ),
                    env_entry(
                        key = "APPLE_SDK_PLATFORM",
                        value = _sdk_name(platform_type, is_simulator),
                    ),
                    env_entry(
                        key = "ZERO_AR_DATE",
                        value = "1",
                    ),
                ] + [env_entry(key = key, value = value) for key, value in ctx.attr.extra_env.items()],
            ),
        ],
    )

    if (ctx.attr.cpu == "ios_arm64" or
        ctx.attr.cpu == "ios_arm64e" or
        ctx.attr.cpu == "ios_x86_64" or
        ctx.attr.cpu == "ios_sim_arm64" or
        ctx.attr.cpu == "tvos_arm64" or
        ctx.attr.cpu == "tvos_x86_64" or
        ctx.attr.cpu == "tvos_sim_arm64" or
        ctx.attr.cpu == "visionos_arm64" or
        ctx.attr.cpu == "visionos_sim_arm64" or
        ctx.attr.cpu == "watchos_arm64_32" or
        ctx.attr.cpu == "watchos_device_arm64" or
        ctx.attr.cpu == "watchos_device_arm64e" or
        ctx.attr.cpu == "watchos_armv7k" or
        ctx.attr.cpu == "watchos_x86_64" or
        ctx.attr.cpu == "watchos_arm64"):
        apply_implicit_frameworks_feature = feature(
            name = "apply_implicit_frameworks",
            enabled = True,
            flag_sets = [
                flag_set(
                    actions = _DYNAMIC_LINK_ACTIONS,
                    flag_groups = [
                        flag_group(
                            flags = ["-framework", "Foundation", "-framework", "UIKit"],
                        ),
                    ],
                ),
            ],
        )
    elif (ctx.attr.cpu == "darwin_x86_64" or
          ctx.attr.cpu == "darwin_arm64" or
          ctx.attr.cpu == "darwin_arm64e"):
        apply_implicit_frameworks_feature = feature(
            name = "apply_implicit_frameworks",
            enabled = True,
            flag_sets = [
                flag_set(
                    actions = _DYNAMIC_LINK_ACTIONS,
                    flag_groups = [flag_group(flags = ["-framework", "Foundation"])],
                    with_features = [with_feature_set(not_features = ["kernel_extension"])],
                ),
            ],
        )
    else:
        apply_implicit_frameworks_feature = feature(name = "apply_implicit_frameworks")

    random_seed_feature = feature(
        name = "random_seed",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.cpp_module_codegen,
                    ACTION_NAMES.cpp_module_compile,
                ],
                flag_groups = [
                    flag_group(
                        flags = ["-frandom-seed=%{output_file}"],
                        expand_if_available = "output_file",
                    ),
                ],
            ),
        ],
    )

    llvm_coverage_map_format_feature = feature(
        name = "llvm_coverage_map_format",
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.preprocess_assemble,
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.cpp_module_compile,
                    ACTION_NAMES.objc_compile,
                    ACTION_NAMES.objcpp_compile,
                ],
                flag_groups = [
                    flag_group(
                        flags = ["-fprofile-instr-generate", "-fcoverage-mapping", "-g"],
                    ),
                ],
            ),
            flag_set(
                actions = _DYNAMIC_LINK_ACTIONS,
                flag_groups = [flag_group(flags = ["-fprofile-instr-generate"])],
            ),
        ],
        requires = [feature_set(features = ["coverage"])],
    )

    coverage_prefix_map_feature = feature(
        name = "coverage_prefix_map",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.preprocess_assemble,
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.cpp_module_compile,
                    ACTION_NAMES.objc_compile,
                    ACTION_NAMES.objcpp_compile,
                ],
                flag_groups = [
                    flag_group(
                        flags = ["-fcoverage-prefix-map=__BAZEL_EXECUTION_ROOT__=."],
                    ),
                ],
            ),
        ],
        requires = [feature_set(features = ["coverage"])],
    )

    force_pic_flags_feature = feature(
        name = "force_pic_flags",
        flag_sets = [
            flag_set(
                actions = [ACTION_NAMES.cpp_link_executable],
                flag_groups = [
                    flag_group(
                        flags = ["-Wl,-pie"],
                        expand_if_available = "force_pic",
                    ),
                ],
            ),
        ],
    )

    sysroot_feature = feature(
        name = "sysroot",
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.assemble,
                    ACTION_NAMES.preprocess_assemble,
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.cpp_module_compile,
                    ACTION_NAMES.objc_compile,
                    ACTION_NAMES.objcpp_compile,
                    ACTION_NAMES.cpp_header_parsing,
                    ACTION_NAMES.cpp_link_executable,
                    ACTION_NAMES.cpp_link_dynamic_library,
                    ACTION_NAMES.cpp_link_nodeps_dynamic_library,
                    ACTION_NAMES.linkstamp_compile,
                    ACTION_NAMES.clif_match,
                ],
                flag_groups = [
                    flag_group(
                        flags = ["--sysroot=%{sysroot}"],
                        expand_if_available = "sysroot",
                    ),
                ],
            ),
        ],
    )

    autofdo_feature = feature(
        name = "autofdo",
        flag_sets = [
            flag_set(
                actions = [ACTION_NAMES.c_compile, ACTION_NAMES.cpp_compile],
                flag_groups = [
                    flag_group(
                        flags = [
                            "-fauto-profile=%{fdo_profile_path}",
                            "-fprofile-correction",
                        ],
                        expand_if_available = "fdo_profile_path",
                    ),
                ],
            ),
        ],
        provides = ["profile"],
    )

    link_libcpp_feature = feature(
        name = "link_libc++",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = _DYNAMIC_LINK_ACTIONS,
                flag_groups = [flag_group(flags = ["-lc++"])],
                with_features = [with_feature_set(not_features = ["kernel_extension"])],
            ),
        ],
    )

    objc_actions_feature = feature(
        name = "objc_actions",
        implies = [
            "objc-compile",
            "objc++-compile",
            "objc-fully-link",
            _OBJC_ARCHIVE_ACTION_NAME,
            "objc-executable",
            _OBJCPP_EXECUTABLE_ACTION_NAME,
            "assemble",
            "preprocess-assemble",
            "c-compile",
            "c++-compile",
            "c++-link-static-library",
            "c++-link-dynamic-library",
            "c++-link-nodeps-dynamic-library",
            "c++-link-executable",
        ],
    )

    unfiltered_compile_flags_feature = feature(
        name = "unfiltered_compile_flags",
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.assemble,
                    ACTION_NAMES.preprocess_assemble,
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.cpp_header_parsing,
                    ACTION_NAMES.cpp_module_compile,
                    ACTION_NAMES.cpp_module_codegen,
                    ACTION_NAMES.linkstamp_compile,
                ],
                flag_groups = [
                    flag_group(
                        flags = [
                            "-no-canonical-prefixes",
                            "-Wno-builtin-macro-redefined",
                            "-D__DATE__=\"redacted\"",
                            "-D__TIMESTAMP__=\"redacted\"",
                            "-D__TIME__=\"redacted\"",
                            "-target",
                            target_system_name,
                        ],
                    ),
                ],
            ),
        ],
    )

    linker_param_file_feature = feature(
        name = "linker_param_file",
        flag_sets = [
            flag_set(
                actions = _DYNAMIC_LINK_ACTIONS + [
                    ACTION_NAMES.cpp_link_static_library,
                    _OBJC_ARCHIVE_ACTION_NAME,
                    ACTION_NAMES.objc_fully_link,
                ],
                flag_groups = [
                    flag_group(
                        flags = ["@%{linker_param_file}"],
                        expand_if_available = "linker_param_file",
                    ),
                ],
            ),
        ],
    )

    relative_ast_path_feature = feature(
        name = "relative_ast_path",
        enabled = True,
        env_sets = [
            env_set(
                actions = _DYNAMIC_LINK_ACTIONS,
                env_entries = [
                    env_entry(
                        key = "RELATIVE_AST_PATH",
                        value = "true",
                    ),
                ],
            ),
        ],
    )

    archiver_flags_feature = feature(
        name = "archiver_flags",
        flag_sets = [
            flag_set(
                actions = [ACTION_NAMES.cpp_link_static_library],
                flag_groups = [
                    flag_group(
                        flags = [
                            "-D",
                            "-no_warning_for_no_symbols",
                            "-static",
                            "-o",
                            "%{output_execpath}",
                        ],
                        expand_if_available = "output_execpath",
                    ),
                ],
            ),
        ],
    )

    fdo_optimize_feature = feature(
        name = "fdo_optimize",
        flag_sets = [
            flag_set(
                actions = [ACTION_NAMES.c_compile, ACTION_NAMES.cpp_compile],
                flag_groups = [
                    flag_group(
                        flags = [
                            "-fprofile-use=%{fdo_profile_path}",
                            "-Wno-profile-instr-unprofiled",
                            "-Wno-profile-instr-out-of-date",
                            "-fprofile-correction",
                        ],
                        expand_if_available = "fdo_profile_path",
                    ),
                ],
            ),
        ],
        provides = ["profile"],
    )

    no_objc_arc_feature = feature(
        name = "no_objc_arc",
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.cpp_module_compile,
                    ACTION_NAMES.cpp_header_parsing,
                    ACTION_NAMES.assemble,
                    ACTION_NAMES.preprocess_assemble,
                    ACTION_NAMES.objc_compile,
                    ACTION_NAMES.objcpp_compile,
                ],
                flag_groups = [
                    flag_group(
                        flags = ["-fno-objc-arc"],
                        expand_if_available = "no_objc_arc",
                    ),
                ],
            ),
        ],
    )

    debug_prefix_map_pwd_is_dot_feature = feature(
        name = "debug_prefix_map_pwd_is_dot",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.assemble,
                    ACTION_NAMES.preprocess_assemble,
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.cpp_header_parsing,
                    ACTION_NAMES.cpp_module_compile,
                    ACTION_NAMES.cpp_module_codegen,
                    ACTION_NAMES.linkstamp_compile,
                    ACTION_NAMES.objc_compile,
                    ACTION_NAMES.objcpp_compile,
                ],
                flag_groups = [flag_group(flags = ["-fdebug-prefix-map=__BAZEL_EXECUTION_ROOT__=."])],
            ),
        ],
    )

    remap_xcode_path_feature = feature(
        name = "remap_xcode_path",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.assemble,
                    ACTION_NAMES.preprocess_assemble,
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.cpp_header_parsing,
                    ACTION_NAMES.cpp_module_compile,
                    ACTION_NAMES.cpp_module_codegen,
                    ACTION_NAMES.linkstamp_compile,
                    ACTION_NAMES.objc_compile,
                    ACTION_NAMES.objcpp_compile,
                ],
                flag_groups = [flag_group(flags = [
                    "-fdebug-prefix-map=__BAZEL_XCODE_DEVELOPER_DIR__=/PLACEHOLDER_DEVELOPER_DIR",
                ])],
            ),
        ],
    )

    linkstamps_feature = feature(
        name = "linkstamps",
        flag_sets = [
            flag_set(
                actions = all_link_actions,
                flag_groups = [
                    flag_group(
                        flags = ["%{linkstamp_paths}"],
                        iterate_over = "linkstamp_paths",
                        expand_if_available = "linkstamp_paths",
                    ),
                ],
            ),
        ],
    )

    include_paths_feature = feature(
        name = "include_paths",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.preprocess_assemble,
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.cpp_header_parsing,
                    ACTION_NAMES.cpp_module_compile,
                    ACTION_NAMES.linkstamp_compile,
                    ACTION_NAMES.clif_match,
                    ACTION_NAMES.objc_compile,
                    ACTION_NAMES.objcpp_compile,
                ],
                flag_groups = [
                    flag_group(
                        flags = ["-iquote", "%{quote_include_paths}"],
                        iterate_over = "quote_include_paths",
                    ),
                    flag_group(
                        flags = ["-I%{include_paths}"],
                        iterate_over = "include_paths",
                    ),
                    flag_group(
                        flags = ["-isystem", "%{system_include_paths}"],
                        iterate_over = "system_include_paths",
                    ),
                ],
            ),
        ],
    )

    default_compile_flags_feature = feature(
        name = "default_compile_flags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.assemble,
                    ACTION_NAMES.preprocess_assemble,
                    ACTION_NAMES.linkstamp_compile,
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.cpp_header_parsing,
                    ACTION_NAMES.cpp_module_compile,
                    ACTION_NAMES.cpp_module_codegen,
                    ACTION_NAMES.lto_backend,
                    ACTION_NAMES.clif_match,
                    ACTION_NAMES.objc_compile,
                    ACTION_NAMES.objcpp_compile,
                ],
                flag_groups = [
                    flag_group(
                        flags = [
                            "-D_FORTIFY_SOURCE=1",
                        ],
                    ),
                ],
                with_features = [with_feature_set(not_features = ["asan"])],
            ),
            flag_set(
                actions = [
                    ACTION_NAMES.assemble,
                    ACTION_NAMES.preprocess_assemble,
                    ACTION_NAMES.linkstamp_compile,
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.cpp_header_parsing,
                    ACTION_NAMES.cpp_module_compile,
                    ACTION_NAMES.cpp_module_codegen,
                    ACTION_NAMES.lto_backend,
                    ACTION_NAMES.clif_match,
                    ACTION_NAMES.objc_compile,
                    ACTION_NAMES.objcpp_compile,
                ],
                flag_groups = [
                    flag_group(
                        flags = [
                            "-fstack-protector",
                            "-fcolor-diagnostics",
                            "-Wall",
                            "-Wthread-safety",
                            "-Wself-assign",
                            "-fno-omit-frame-pointer",
                        ],
                    ),
                ],
            ),
            flag_set(
                actions = [
                    ACTION_NAMES.assemble,
                    ACTION_NAMES.preprocess_assemble,
                    ACTION_NAMES.linkstamp_compile,
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.cpp_header_parsing,
                    ACTION_NAMES.cpp_module_compile,
                    ACTION_NAMES.cpp_module_codegen,
                    ACTION_NAMES.lto_backend,
                    ACTION_NAMES.clif_match,
                    ACTION_NAMES.objc_compile,
                    ACTION_NAMES.objcpp_compile,
                ],
                flag_groups = [flag_group(flags = ["-g2"])],
                with_features = [with_feature_set(features = ["dbg"])],
            ),
            flag_set(
                actions = all_c_compile_actions,
                flag_groups = [
                    flag_group(
                        flags = [
                            "-std=c11",
                        ],
                    ),
                ],
            ),
            flag_set(
                actions = [
                    ACTION_NAMES.linkstamp_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.cpp_header_parsing,
                    ACTION_NAMES.cpp_module_compile,
                    ACTION_NAMES.cpp_module_codegen,
                    ACTION_NAMES.lto_backend,
                    ACTION_NAMES.clif_match,
                ],
                flag_groups = [flag_group(flags = ["-std=c++20"])],
            ),
        ],
    )

    ns_block_assertions_feature = feature(
        name = "ns_block_assertions",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.assemble,
                    ACTION_NAMES.preprocess_assemble,
                    ACTION_NAMES.linkstamp_compile,
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.cpp_header_parsing,
                    ACTION_NAMES.cpp_module_compile,
                    ACTION_NAMES.cpp_module_codegen,
                    ACTION_NAMES.lto_backend,
                    ACTION_NAMES.clif_match,
                    ACTION_NAMES.objc_compile,
                    ACTION_NAMES.objcpp_compile,
                ],
                flag_groups = [flag_group(flags = ["-DNS_BLOCK_ASSERTIONS=1"])],
                with_features = [with_feature_set(features = ["opt"])],
            ),
        ],
    )

    objcopy_embed_flags_feature = feature(
        name = "objcopy_embed_flags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = ["objcopy_embed_data"],
                flag_groups = [flag_group(flags = ["-I", "binary"])],
            ),
        ],
    )

    dead_strip_feature = feature(
        name = "dead_strip",
        flag_sets = [
            flag_set(
                actions = _DYNAMIC_LINK_ACTIONS,
                flag_groups = [flag_group(flags = ["-dead_strip"])],
            ),
        ],
    )

    oso_prefix_feature = feature(
        name = "oso_prefix_is_pwd",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = _DYNAMIC_LINK_ACTIONS,
                flag_groups = [flag_group(flags = ["-Wl,-oso_prefix,__BAZEL_EXECUTION_ROOT_NO_SANDBOX__/"])],
            ),
        ],
    )

    generate_dsym_file_feature = feature(
        name = "generate_dsym_file",
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.objc_compile,
                    ACTION_NAMES.objcpp_compile,
                    "objc-executable",
                    _OBJCPP_EXECUTABLE_ACTION_NAME,
                ],
                flag_groups = [flag_group(flags = ["-g"])],
            ),
            flag_set(
                actions = ["objc-executable", _OBJCPP_EXECUTABLE_ACTION_NAME],
                flag_groups = [
                    flag_group(
                        flags = [
                            "DSYM_HINT_LINKED_BINARY=%{linked_binary}",
                            "DSYM_HINT_DSYM_PATH=%{dsym_path}",
                        ],
                    ),
                ],
            ),
        ],
    )

    # Kernel extensions for Apple Silicon are arm64e.
    if (ctx.attr.cpu == "darwin_x86_64" or
        ctx.attr.cpu == "darwin_arm64e"):
        kernel_extension_feature = feature(
            name = "kernel_extension",
            flag_sets = [
                flag_set(
                    actions = ["objc-executable", _OBJCPP_EXECUTABLE_ACTION_NAME],
                    flag_groups = [
                        flag_group(
                            flags = [
                                "-nostdlib",
                                "-lkmod",
                                "-lkmodc++",
                                "-lcc_kext",
                                "-Xlinker",
                                "-kext",
                            ],
                        ),
                    ],
                ),
            ],
        )
    else:
        kernel_extension_feature = feature(name = "kernel_extension")

    apply_default_warnings_feature = feature(
        name = "apply_default_warnings",
        flag_sets = [
            flag_set(
                actions = [ACTION_NAMES.objc_compile, ACTION_NAMES.objcpp_compile],
                flag_groups = [
                    flag_group(
                        flags = [
                            "-Werror=incompatible-sysroot",
                            "-Wshorten-64-to-32",
                            "-Wbool-conversion",
                            "-Wconstant-conversion",
                            "-Wduplicate-method-match",
                            "-Wempty-body",
                            "-Wenum-conversion",
                            "-Wint-conversion",
                            "-Wunreachable-code",
                            "-Wmismatched-return-types",
                            "-Wundeclared-selector",
                            "-Wuninitialized",
                            "-Wunused-function",
                            "-Wunused-variable",
                        ],
                    ),
                ],
            ),
        ],
    )

    dependency_file_feature = feature(
        name = "dependency_file",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.assemble,
                    ACTION_NAMES.preprocess_assemble,
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.cpp_module_compile,
                    ACTION_NAMES.objc_compile,
                    ACTION_NAMES.objcpp_compile,
                    ACTION_NAMES.cpp_header_parsing,
                ],
                flag_groups = [
                    flag_group(
                        flags = ["-MD", "-MF", "%{dependency_file}"],
                        expand_if_available = "dependency_file",
                    ),
                ],
            ),
        ],
    )

    serialized_diagnostics_file_feature = feature(
        name = "serialized_diagnostics_file",
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.assemble,
                    ACTION_NAMES.preprocess_assemble,
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.cpp_module_compile,
                    ACTION_NAMES.objc_compile,
                    ACTION_NAMES.objcpp_compile,
                    ACTION_NAMES.cpp_header_parsing,
                ],
                flag_groups = [
                    flag_group(
                        flags = ["--serialize-diagnostics", "%{serialized_diagnostics_file}"],
                        expand_if_available = "serialized_diagnostics_file",
                    ),
                ],
            ),
        ],
    )

    preprocessor_defines_feature = feature(
        name = "preprocessor_defines",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.preprocess_assemble,
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.cpp_header_parsing,
                    ACTION_NAMES.cpp_module_compile,
                    ACTION_NAMES.linkstamp_compile,
                    ACTION_NAMES.objc_compile,
                    ACTION_NAMES.objcpp_compile,
                ],
                flag_groups = [
                    flag_group(
                        flags = ["-D%{preprocessor_defines}"],
                        iterate_over = "preprocessor_defines",
                    ),
                ],
            ),
        ],
    )

    fdo_instrument_feature = feature(
        name = "fdo_instrument",
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.cpp_link_dynamic_library,
                    ACTION_NAMES.cpp_link_nodeps_dynamic_library,
                    ACTION_NAMES.cpp_link_executable,
                ],
                flag_groups = [
                    flag_group(
                        flags = [
                            "-fprofile-generate=%{fdo_instrument_path}",
                            "-fno-data-sections",
                        ],
                        expand_if_available = "fdo_instrument_path",
                    ),
                ],
            ),
        ],
        provides = ["profile"],
    )

    if (ctx.attr.cpu == "darwin_x86_64" or
        ctx.attr.cpu == "darwin_arm64" or
        ctx.attr.cpu == "darwin_arm64e"):
        link_cocoa_feature = feature(
            name = "link_cocoa",
            flag_sets = [
                flag_set(
                    actions = ["objc-executable", _OBJCPP_EXECUTABLE_ACTION_NAME],
                    flag_groups = [flag_group(flags = ["-framework", "Cocoa"])],
                ),
            ],
        )
    else:
        link_cocoa_feature = feature(name = "link_cocoa")

    user_compile_flags_feature = feature(
        name = "user_compile_flags",
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.assemble,
                    ACTION_NAMES.preprocess_assemble,
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.cpp_header_parsing,
                    ACTION_NAMES.cpp_module_compile,
                    ACTION_NAMES.cpp_module_codegen,
                    ACTION_NAMES.linkstamp_compile,
                    ACTION_NAMES.objc_compile,
                    ACTION_NAMES.objcpp_compile,
                ],
                flag_groups = [
                    flag_group(
                        flags = ["%{user_compile_flags}"],
                        iterate_over = "user_compile_flags",
                        expand_if_available = "user_compile_flags",
                    ),
                ],
            ),
        ],
    )

    headerpad_feature = feature(
        name = "headerpad",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = _DYNAMIC_LINK_ACTIONS,
                flag_groups = [flag_group(flags = ["-headerpad_max_install_names"])],
            ),
        ],
    )

    generate_linkmap_feature = feature(
        name = "generate_linkmap",
        flag_sets = [
            flag_set(
                actions = _DYNAMIC_LINK_ACTIONS,
                flag_groups = [
                    flag_group(
                        flags = [
                            "-Xlinker",
                            "-map",
                            "-Xlinker",
                            "%{linkmap_exec_path}",
                        ],
                        expand_if_available = "linkmap_exec_path",
                    ),
                ],
            ),
        ],
    )

    set_install_name = feature(
        name = "set_install_name",
        enabled = ctx.fragments.cpp.do_not_use_macos_set_install_name,
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.cpp_link_dynamic_library,
                    ACTION_NAMES.cpp_link_nodeps_dynamic_library,
                ],
                flag_groups = [
                    flag_group(
                        flags = [
                            "-Xlinker",
                            "-install_name",
                            "-Xlinker",
                            "@rpath/%{runtime_solib_name}",
                        ],
                        expand_if_available = "runtime_solib_name",
                    ),
                ],
            ),
        ],
    )

    asan_feature = feature(
        name = "asan",
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.objc_compile,
                    ACTION_NAMES.objcpp_compile,
                ],
                flag_groups = [
                    flag_group(flags = ["-fsanitize=address"]),
                ],
                with_features = [
                    with_feature_set(features = ["asan"]),
                ],
            ),
            flag_set(
                actions = _DYNAMIC_LINK_ACTIONS,
                flag_groups = [
                    flag_group(flags = ["-fsanitize=address"]),
                ],
                with_features = [
                    with_feature_set(features = ["asan"]),
                ],
            ),
        ],
    )

    tsan_feature = feature(
        name = "tsan",
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.objc_compile,
                    ACTION_NAMES.objcpp_compile,
                ],
                flag_groups = [
                    flag_group(flags = ["-fsanitize=thread"]),
                ],
                with_features = [
                    with_feature_set(features = ["tsan"]),
                ],
            ),
            flag_set(
                actions = _DYNAMIC_LINK_ACTIONS,
                flag_groups = [
                    flag_group(flags = ["-fsanitize=thread"]),
                ],
                with_features = [
                    with_feature_set(features = ["tsan"]),
                ],
            ),
        ],
    )

    ubsan_feature = feature(
        name = "ubsan",
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.objc_compile,
                    ACTION_NAMES.objcpp_compile,
                ],
                flag_groups = [
                    flag_group(flags = ["-fsanitize=undefined"]),
                ],
                with_features = [
                    with_feature_set(features = ["ubsan"]),
                ],
            ),
            flag_set(
                actions = _DYNAMIC_LINK_ACTIONS,
                flag_groups = [
                    flag_group(flags = ["-fsanitize=undefined"]),
                ],
                with_features = [
                    with_feature_set(features = ["ubsan"]),
                ],
            ),
        ],
    )

    default_sanitizer_flags_feature = feature(
        name = "default_sanitizer_flags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.objc_compile,
                    ACTION_NAMES.objcpp_compile,
                ],
                flag_groups = [
                    flag_group(
                        flags = [
                            "-gline-tables-only",
                            "-fno-omit-frame-pointer",
                            "-fno-sanitize-recover=all",
                        ],
                    ),
                ],
                with_features = [
                    with_feature_set(features = ["asan"]),
                    with_feature_set(features = ["tsan"]),
                    with_feature_set(features = ["ubsan"]),
                ],
            ),
        ],
    )

    treat_warnings_as_errors_feature = feature(
        name = "treat_warnings_as_errors",
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.objc_compile,
                    ACTION_NAMES.objcpp_compile,
                ],
                flag_groups = [flag_group(flags = ["-Werror"])],
            ),
            flag_set(
                actions = _DYNAMIC_LINK_ACTIONS,
                flag_groups = [flag_group(flags = ["-Wl,-fatal_warnings"])],
            ),
        ],
    )

    # As of Xcode 15, linker warnings are emitted if duplicate `-l` options are
    # present. Until such linkopts can be deduped by bazel itself, we disable
    # these warnings.
    no_warn_duplicate_libraries_feature = feature(
        name = "no_warn_duplicate_libraries",
        enabled = "no_warn_duplicate_libraries" in ctx.features,
        flag_sets = [
            flag_set(
                actions = _DYNAMIC_LINK_ACTIONS,
                flag_groups = [
                    flag_group(
                        flags = [
                            "-Wl,-no_warn_duplicate_libraries",
                        ],
                    ),
                ],
            ),
        ],
    )

    modulemaps = ctx.attr.module_map.files.to_list()
    if modulemaps:
        if len(modulemaps) != 1:
            fail("internal error: expected 1 modulemap got:", modulemaps)
        layering_check_feature = feature(
            name = "layering_check",
            flag_sets = [
                flag_set(
                    actions = [
                        ACTION_NAMES.c_compile,
                        ACTION_NAMES.cpp_compile,
                        ACTION_NAMES.cpp_header_parsing,
                        ACTION_NAMES.cpp_module_compile,
                        ACTION_NAMES.objc_compile,
                        ACTION_NAMES.objcpp_compile,
                    ],
                    flag_groups = [
                        flag_group(
                            flags = [
                                "-fmodules-strict-decluse",
                                "-Wprivate-header",
                                "-Xclang",
                                "-fmodule-name=%{module_name}",
                                "-Xclang",
                                "-fmodule-map-file=%{module_map_file}",
                            ],
                        ),
                        flag_group(
                            iterate_over = "dependent_module_map_files",
                            flags = [
                                "-Xclang",
                                "-fmodule-map-file=%{dependent_module_map_files}",
                            ],
                        ),
                    ],
                ),
            ],
            env_sets = [
                env_set(
                    actions = [
                        ACTION_NAMES.c_compile,
                        ACTION_NAMES.cpp_compile,
                        ACTION_NAMES.cpp_header_parsing,
                        ACTION_NAMES.cpp_module_compile,
                        ACTION_NAMES.objc_compile,
                        ACTION_NAMES.objcpp_compile,
                    ],
                    env_entries = [
                        env_entry(
                            key = "APPLE_SUPPORT_MODULEMAP",
                            value = modulemaps[0].path,
                        ),
                    ],
                ),
            ],
        )
    else:
        layering_check_feature = feature(name = "layering_check")

    features = [
        # Marker features
        feature(name = "archive_param_file", enabled = True),
        feature(name = "compile_all_modules"),
        feature(name = "coverage"),
        feature(name = "dbg"),
        feature(name = "exclude_private_headers_in_module_maps"),
        feature(name = "fastbuild"),
        feature(name = "has_configured_linker_path"),
        feature(name = "module_maps", enabled = True),
        feature(name = "no_legacy_features"),
        feature(name = "only_doth_headers_in_module_maps"),
        feature(name = "opt"),
        feature(name = "parse_headers"),

        # Features with more configuration
        link_libcpp_feature,
        default_compile_flags_feature,
        ns_block_assertions_feature,
        debug_prefix_map_pwd_is_dot_feature,
        remap_xcode_path_feature,
        generate_dsym_file_feature,
        generate_linkmap_feature,
        oso_prefix_feature,
        objc_actions_feature,
        strip_debug_symbols_feature,
        shared_flag_feature,
        kernel_extension_feature,
        linkstamps_feature,
        output_execpath_flags_feature,
        archiver_flags_feature,
        runtime_root_flags_feature,
        input_param_flags_feature,
        objc_link_flag_feature,
        force_pic_flags_feature,
        pch_feature,
        apply_default_warnings_feature,
        includes_feature,
        include_paths_feature,
        sysroot_feature,
        dependency_file_feature,
        serialized_diagnostics_file_feature,
        pic_feature,
        per_object_debug_info_feature,
        preprocessor_defines_feature,
        framework_paths_feature,
        random_seed_feature,
        fdo_instrument_feature,
        fdo_optimize_feature,
        autofdo_feature,
        lipo_feature,
        llvm_coverage_map_format_feature,
        gcc_coverage_map_format_feature,
        coverage_prefix_map_feature,
        apply_default_compiler_flags_feature,
        include_system_dirs_feature,
        headerpad_feature,
        objc_arc_feature,
        no_objc_arc_feature,
        apple_env_feature,
        relative_ast_path_feature,
        gcc_quoting_for_param_files_feature,
        user_link_flags_feature,
        ###
        # mongodb customization
        mongo_frameworks_feature,
        ###
        default_link_flags_feature,
        no_deduplicate_feature,
        dead_strip_feature,
        apply_implicit_frameworks_feature,
        link_cocoa_feature,
        apply_simulator_compiler_flags_feature,
        unfiltered_cxx_flags_feature,
        user_compile_flags_feature,
        unfiltered_compile_flags_feature,
        linker_param_file_feature,
        compiler_input_flags_feature,
        compiler_output_flags_feature,
        objcopy_embed_flags_feature,
        set_install_name,
        asan_feature,
        tsan_feature,
        ubsan_feature,
        default_sanitizer_flags_feature,
        treat_warnings_as_errors_feature,
        no_warn_duplicate_libraries_feature,
        layering_check_feature,
        external_include_paths_feature,
        header_parsing_env_feature,
    ]

    if (ctx.attr.cpu == "darwin_x86_64" or
        ctx.attr.cpu == "darwin_arm64" or
        ctx.attr.cpu == "darwin_arm64e"):
        features.append(feature(name = "dynamic_linking_mode"))

    # macOS artifact name patterns differ from the defaults only for dynamic
    # libraries.
    artifact_name_patterns = [
        artifact_name_pattern(
            category_name = "dynamic_library",
            prefix = "lib",
            extension = ".dylib",
        ),
    ]

    make_variables = [
        make_variable(
            name = "STACK_FRAME_UNLIMITED",
            value = "-Wframe-larger-than=100000000 -Wno-vla",
        ),
    ]

    tool_paths = {
        "ar": "libtool",
        "cpp": "/usr/bin/cpp",
        "dwp": "/usr/bin/dwp",
        "gcc": "cc_wrapper.sh",
        "gcov": "/usr/bin/gcov",
        "ld": "/usr/bin/ld",
        "nm": "/usr/bin/nm",
        "objcopy": "/usr/bin/objcopy",
        "objdump": "/usr/bin/objdump",
        "strip": "/usr/bin/strip",
    }

    tool_paths.update(ctx.attr.tool_paths_overrides)

    out = ctx.actions.declare_file(ctx.label.name)
    ctx.actions.write(out, "Fake executable")
    return [
        cc_common.create_cc_toolchain_config_info(
            ctx = ctx,
            features = features,
            action_configs = action_configs,
            artifact_name_patterns = artifact_name_patterns,
            cxx_builtin_include_directories = ctx.attr.cxx_builtin_include_directories,
            toolchain_identifier = ctx.attr.cpu,
            host_system_name = "x86_64-apple-macosx",
            target_system_name = target_system_name,
            target_cpu = ctx.attr.cpu,
            target_libc = target_libc,
            compiler = "clang",
            abi_version = abi_version,
            abi_libc_version = abi_libc_version,
            tool_paths = [tool_path(name = name, path = path) for (name, path) in tool_paths.items()],
            make_variables = make_variables,
            builtin_sysroot = None,
        ),
        DefaultInfo(
            executable = out,
        ),
    ]

cc_toolchain_config = rule(
    implementation = _impl,
    attrs = {
        "cpu": attr.string(mandatory = True),
        "cxx_builtin_include_directories": attr.string_list(),
        "tool_paths_overrides": attr.string_dict(),
        "extra_env": attr.string_dict(),
        "module_map": attr.label(),
        "_xcode_config": attr.label(default = configuration_field(
            fragment = "apple",
            name = "xcode_config_label",
        )),
    },
    provides = [CcToolchainConfigInfo],
    executable = True,
    fragments = [
        "apple",
        "cpp",
    ],
)
