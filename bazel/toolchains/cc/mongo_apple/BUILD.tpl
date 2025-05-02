###
# This file was retrieved from the following location:
#    https://github.com/bazelbuild/apple_support/blob/2241caed4182e22071647b079c632004699a34e9/crosstool/BUILD.tpl 
###

package(default_visibility = ["//visibility:public"])

load("@build_bazel_apple_support//configs:platforms.bzl", "APPLE_PLATFORMS_CONSTRAINTS")
load(":cc_toolchain_config.bzl", "cc_toolchain_config")
###
# mongo customization
load(
    "@//bazel/toolchains/cc:mongo_custom_features.bzl",
    "COMPILERS",
    "FEATURES_ATTR_NAMES",
    "get_common_features_attrs")
###

_APPLE_ARCHS = APPLE_PLATFORMS_CONSTRAINTS.keys()

CC_TOOLCHAINS = [(
    cpu + "|clang",
    ":cc-compiler-" + cpu,
) for cpu in _APPLE_ARCHS] + [(
    cpu,
    ":cc-compiler-" + cpu,
) for cpu in _APPLE_ARCHS] + [
    ("k8|clang", ":cc-compiler-darwin_x86_64"),
    ("darwin|clang", ":cc-compiler-darwin_x86_64"),
    ("k8", ":cc-compiler-darwin_x86_64"),
    ("darwin", ":cc-compiler-darwin_x86_64"),
]

cc_library(
    name = "link_extra_lib",
)

cc_library(
    name = "malloc",
)

filegroup(
    name = "empty",
    srcs = [],
)

filegroup(
    name = "cc_wrapper",
    srcs = ["cc_wrapper.sh"],
)

cc_toolchain_suite(
    name = "toolchain",
    toolchains = dict(CC_TOOLCHAINS),
)

filegroup(
    name = "modulemap",
    srcs = [
%{layering_check_modulemap}
    ],
)

filegroup(
    name = "tools",
    srcs = [
        ":cc_wrapper",
        ":libtool",
        ":libtool_check_unique",
        ":make_hashed_objlist.py",
        ":modulemap",
        ":wrapped_clang",
        ":wrapped_clang_pp",
        ":xcrunwrapper.sh",
    ],
)

[
    cc_toolchain(
        name = "cc-compiler-" + arch,
        all_files = ":tools",
        ar_files = ":tools",
        as_files = ":tools",
        compiler_files = ":tools",
        dwp_files = ":empty",
        linker_files = ":tools",
        objcopy_files = ":empty",
        strip_files = ":tools",
        supports_header_parsing = 1,
        supports_param_files = 1,
        toolchain_config = arch,
        toolchain_identifier = arch,
        module_map = %{placeholder_modulemap},
    )
    for arch in _APPLE_ARCHS
]

###
# mongo customization
feature_attrs = get_common_features_attrs()
###

[
    cc_toolchain_config(
        name = arch,
        cpu = arch,
        features = [
%{features}
        ],
        cxx_builtin_include_directories = [
%{cxx_builtin_include_directories}
        ],
        tool_paths_overrides = {%{tool_paths_overrides}},
        module_map = ":modulemap",
###
# mongo customization
        compiler = COMPILERS.CLANG,
        optimization_level = feature_attrs[FEATURES_ATTR_NAMES.OPT_LEVEL],
###
    )
    for arch in _APPLE_ARCHS
]
