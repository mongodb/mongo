load("@bazel_tools//tools/cpp:toolchain_utils.bzl", "find_cpp_toolchain")

WITH_DEBUG_SUFFIX = "_with_debug"
CC_SHARED_LIBRARY_SUFFIX = "_shared"

def get_inputs_and_outputs(ctx, shared_ext, static_ext, debug_ext):
    """
    Determines and generates the inputs and outputs.
    Inputs are the extensions for shared libraries, static libraries, and debug files for a given platform.
    Outputs are:
        input_bin: the shared or program binary that contains debug info
                Note it is possible this a static library build in which case input_bin can be None
        output_bin: The output shared or program binary with the debug info to be stripped
                    Note it is possible this a static library build in which case output_bin can be None
                    Note if separate_debug is disabled this will just be a symlink which still contains the debug info
        debug_info: If this is a separate_debug build, this will be the output file which will be the extracted debug info
        static_lib: The static library if this build is building static libraries. This will be a symlink
    """
    shared_lib = None
    static_lib = None
    if ctx.attr.type == "library":
        for file in ctx.attr.binary_with_debug.files.to_list():
            if file.path.endswith(WITH_DEBUG_SUFFIX + static_ext):
                static_lib = file

        if ctx.attr.cc_shared_library != None:
            for file in ctx.attr.cc_shared_library.files.to_list():
                if file.path.endswith(WITH_DEBUG_SUFFIX + shared_ext):
                    shared_lib = file

        if shared_lib:
            basename = shared_lib.basename[:-len(WITH_DEBUG_SUFFIX + shared_ext + CC_SHARED_LIBRARY_SUFFIX)]
            if ctx.attr.enabled:
                debug_info = ctx.actions.declare_file(basename + shared_ext + debug_ext)
            else:
                debug_info = None
            output_bin = ctx.actions.declare_file(basename + shared_ext)
            input_bin = shared_lib
        else:
            debug_info = None
            output_bin = None
            input_bin = None
    elif ctx.attr.type == "program":
        program_bin = ctx.attr.binary_with_debug.files.to_list()[0]

        basename = program_bin.basename[:-len(WITH_DEBUG_SUFFIX)]
        if ctx.attr.enabled:
            debug_info = ctx.actions.declare_file(basename + debug_ext)
        else:
            debug_info = None
        output_bin = ctx.actions.declare_file(basename)
        input_bin = program_bin
    else:
        fail("Can't extract debug info from unknown type: " + ctx.attr.type)

    return input_bin, output_bin, debug_info, static_lib

def propgate_static_lib(ctx, static_lib, static_ext, inputs):
    """
    Static libraries will not have debug info extracts so we symlink to the new target name.
    """
    basename = static_lib.basename[:-len(WITH_DEBUG_SUFFIX + static_ext)]
    unstripped_static_lib = ctx.actions.declare_file(basename + static_ext)

    ctx.actions.symlink(
        output = unstripped_static_lib,
        target_file = static_lib,
    )

    return unstripped_static_lib

def create_new_ccinfo_library(ctx, cc_toolchain, shared_lib, static_lib, cc_shared_library = None):
    """
    We need to create new CcInfo with the new target names, this will take in the newly
    named library files and construct a new CcInfo basically stripping out the "_with_debug"
    name.
    """

    if ctx.attr.type == "library":
        feature_configuration = cc_common.configure_features(
            ctx = ctx,
            cc_toolchain = cc_toolchain,
            requested_features = ctx.features,
            unsupported_features = ctx.disabled_features,
        )

        linker_input = cc_common.create_linker_input(
            owner = ctx.label,
            libraries = depset(direct = [
                cc_common.create_library_to_link(
                    actions = ctx.actions,
                    feature_configuration = feature_configuration,
                    cc_toolchain = cc_toolchain,
                    dynamic_library = shared_lib,
                    static_library = static_lib if cc_shared_library == None else None,
                ),
            ]),
            user_link_flags = ctx.attr.binary_with_debug[CcInfo].linking_context.linker_inputs.to_list()[0].user_link_flags,
        )

        linker_input_deps = []
        for dep in ctx.attr.deps:
            linker_input_deps.append(dep[CcInfo].linking_context.linker_inputs)

        linking_context = cc_common.create_linking_context(linker_inputs = depset(direct = [linker_input], transitive = linker_input_deps))

    else:
        linking_context = ctx.attr.binary_with_debug[CcInfo].linking_context

    return CcInfo(
        compilation_context = ctx.attr.binary_with_debug[CcInfo].compilation_context,
        linking_context = linking_context,
    )

def create_new_cc_shared_library_info(ctx, cc_toolchain, output_shared_lib, original_info, static_lib = None):
    """
    We need to create a CcSharedLibraryInfo to pass to the cc_binary and cc_library that depend on it
    so they know to link the cc_shared_library instead of the associated cc_library.
    name.
    """
    feature_configuration = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = cc_toolchain,
        requested_features = ctx.features,
        unsupported_features = ctx.disabled_features,
    )

    # Loop through all dependencies to include their resulting (shared/debug decorator stripped) shared library files
    # as inputs.
    # TODO(SERVER-85819): Investigate to see if it's possible to merge the depset without looping over all transitive
    # dependencies.
    dep_libraries = []
    for dep in ctx.attr.deps:
        for input in dep[CcInfo].linking_context.linker_inputs.to_list():
            for library in input.libraries:
                dep_libraries.append(library)

    linker_input = cc_common.create_linker_input(
        owner = ctx.label,
        libraries = depset(direct = [
            cc_common.create_library_to_link(
                actions = ctx.actions,
                feature_configuration = feature_configuration,
                cc_toolchain = cc_toolchain,
                # Replace reference to dynamic library with final name
                dynamic_library = output_shared_lib,
                # Omit reference to static library
            ),
        ], transitive = [depset(dep_libraries)]),
        user_link_flags = original_info.linker_input.user_link_flags,
        additional_inputs = depset(original_info.linker_input.additional_inputs),
    )

    return CcSharedLibraryInfo(
        dynamic_deps = original_info.dynamic_deps,
        exports = original_info.exports,
        link_once_static_libs = original_info.link_once_static_libs,
        linker_input = linker_input,
    )

def noop_extraction(ctx):
    return [
        DefaultInfo(
            files = depset(transitive = [ctx.attr.binary_with_debug.files]),
            executable = ctx.attr.binary_with_debug.files[0] if ctx.attr.type == "program" else None,
        ),
        ctx.attr.binary_with_debug[CcInfo],
    ]

def linux_extraction(ctx, cc_toolchain, inputs):
    outputs = []
    input_bin, output_bin, debug_info, static_lib = get_inputs_and_outputs(ctx, ".so", ".a", ".debug")

    if input_bin:
        if ctx.attr.enabled:
            ctx.actions.run(
                executable = cc_toolchain.objcopy_executable,
                outputs = [debug_info],
                inputs = inputs,
                arguments = [
                    "--only-keep-debug",
                    input_bin.path,
                    debug_info.path,
                ],
                mnemonic = "ExtractDebuginfo",
            )

            ctx.actions.run(
                executable = cc_toolchain.objcopy_executable,
                outputs = [output_bin],
                inputs = depset([debug_info], transitive = [inputs]),
                arguments = [
                    "--strip-debug",
                    "--add-gnu-debuglink",
                    debug_info.path,
                    input_bin.path,
                    output_bin.path,
                ],
                mnemonic = "StripDebuginfo",
            )
            outputs += [output_bin, debug_info]
        else:
            ctx.actions.symlink(
                output = output_bin,
                target_file = input_bin,
            )
            outputs += [output_bin]

    unstripped_static_bin = None
    if static_lib:
        unstripped_static_bin = propgate_static_lib(ctx, static_lib, ".a", inputs)
        outputs.append(unstripped_static_bin)

    provided_info = [
        DefaultInfo(
            files = depset(outputs),
            executable = output_bin if ctx.attr.type == "program" else None,
        ),
        create_new_ccinfo_library(ctx, cc_toolchain, output_bin, unstripped_static_bin, ctx.attr.cc_shared_library),
    ]

    if ctx.attr.cc_shared_library != None:
        provided_info.append(
            create_new_cc_shared_library_info(ctx, cc_toolchain, output_bin, ctx.attr.cc_shared_library[CcSharedLibraryInfo], static_lib),
        )

    return provided_info

def macos_extraction(ctx, cc_toolchain, inputs):
    outputs = []
    input_bin, output_bin, debug_info, static_lib = get_inputs_and_outputs(ctx, ".dylib", ".a", ".dSYM")

    if input_bin:
        if ctx.attr.enabled:
            ctx.actions.run(
                executable = "dsymutil",
                outputs = [debug_info],
                inputs = inputs,
                arguments = [
                    "-num-threads",
                    "1",
                    input_bin.path,
                    "-o",
                    debug_info.path,
                ],
                mnemonic = "ExtractDebuginfo",
            )

            ctx.actions.run(
                executable = cc_toolchain.strip_executable,
                outputs = [output_bin],
                inputs = depset([debug_info], transitive = [inputs]),
                arguments = [
                    "-S",
                    "-o",
                    output_bin.path,
                    input_bin.path,
                ],
                mnemonic = "StripDebuginfo",
            )
            outputs += [output_bin, debug_info]
        else:
            ctx.actions.symlink(
                output = output_bin,
                target_file = input_bin,
            )
            outputs += [output_bin]

    unstripped_static_bin = None
    if static_lib:
        unstripped_static_bin = propgate_static_lib(ctx, static_lib, ".a", inputs)
        outputs.append(unstripped_static_bin)

    provided_info = [
        DefaultInfo(
            files = depset(outputs),
            executable = output_bin if ctx.attr.type == "program" else None,
        ),
        create_new_ccinfo_library(ctx, cc_toolchain, output_bin, unstripped_static_bin, ctx.attr.cc_shared_library),
    ]

    if ctx.attr.cc_shared_library != None:
        provided_info.append(
            create_new_cc_shared_library_info(ctx, cc_toolchain, output_bin, ctx.attr.cc_shared_library[CcSharedLibraryInfo]),
        )

    return provided_info

def windows_extraction(ctx, cc_toolchain, inputs):
    if ctx.attr.type == "library":
        ext = ".lib"
    elif ctx.attr.type == "program":
        ext = ".exe"
    else:
        fail("Can't extract debug info from unknown type: " + ctx.attr.type)

    basename = ctx.attr.binary_with_debug.files.to_list()[0].basename[:-len(WITH_DEBUG_SUFFIX + ext)]
    output = ctx.actions.declare_file(basename + ext)

    outputs = []
    output_library = None
    output_dynamic_library = None
    for input in ctx.attr.binary_with_debug.files.to_list():
        ext = "." + input.extension

        basename = input.basename[:-len(WITH_DEBUG_SUFFIX + ext)]
        output = ctx.actions.declare_file(basename + ext)
        outputs.append(output)

        if ext == ".lib":
            output_library = output
        if ext == ".dll":
            output_dynamic_library = output

        ctx.actions.symlink(
            output = output,
            target_file = input,
        )

    provided_info = [
        DefaultInfo(
            files = depset(outputs),
            executable = output if ctx.attr.type == "program" else None,
        ),
        create_new_ccinfo_library(ctx, cc_toolchain, output_dynamic_library, output_library, ctx.attr.cc_shared_library),
    ]

    if ctx.attr.cc_shared_library != None:
        provided_info.append(
            create_new_cc_shared_library_info(ctx, cc_toolchain, output_dynamic_library, ctx.attr.cc_shared_library[CcSharedLibraryInfo]),
        )

    return provided_info

def extract_debuginfo_impl(ctx):
    # some cases (header file groups) there is no input files to do
    # anything with, besides forward things along.
    if not ctx.attr.binary_with_debug.files.to_list():
        return noop_extraction(ctx)

    cc_toolchain = find_cpp_toolchain(ctx)
    inputs = depset(transitive = [
        ctx.attr.binary_with_debug.files,
        ctx.attr.cc_shared_library.files if ctx.attr.cc_shared_library != None else depset([]),
        cc_toolchain.all_files,
    ])

    linux_constraint = ctx.attr._linux_constraint[platform_common.ConstraintValueInfo]
    macos_constraint = ctx.attr._macos_constraint[platform_common.ConstraintValueInfo]
    windows_constraint = ctx.attr._windows_constraint[platform_common.ConstraintValueInfo]

    if ctx.target_platform_has_constraint(linux_constraint):
        return linux_extraction(ctx, cc_toolchain, inputs)
    elif ctx.target_platform_has_constraint(macos_constraint):
        return macos_extraction(ctx, cc_toolchain, inputs)
    elif ctx.target_platform_has_constraint(windows_constraint):
        return windows_extraction(ctx, cc_toolchain, inputs)

extract_debuginfo = rule(
    extract_debuginfo_impl,
    attrs = {
        "binary_with_debug": attr.label(
            doc = "The the binary to extract debuginfo from.",
            allow_files = True,
        ),
        "type": attr.string(
            doc = "Set to either 'library' or 'program' to discern how to extract the info.",
        ),
        "enabled": attr.bool(default = False, doc = "Flag to enable/disable separate debug generation."),
        "deps": attr.label_list(providers = [CcInfo]),
        "cc_shared_library": attr.label(
            doc = "If extracting from a shared library, the target of the cc_shared_library. Otherwise empty.",
            allow_files = True,
        ),
        "_cc_toolchain": attr.label(default = "@bazel_tools//tools/cpp:current_cc_toolchain"),
        "_linux_constraint": attr.label(default = "@platforms//os:linux"),
        "_macos_constraint": attr.label(default = "@platforms//os:macos"),
        "_windows_constraint": attr.label(default = "@platforms//os:windows"),
    },
    doc = "Extract debuginfo into a separate file",
    toolchains = ["@bazel_tools//tools/cpp:toolchain_type"],
    fragments = ["cpp"],
)

extract_debuginfo_binary = rule(
    extract_debuginfo_impl,
    attrs = {
        "binary_with_debug": attr.label(
            doc = "The the binary to extract debuginfo from.",
            allow_files = True,
        ),
        "type": attr.string(
            doc = "Set to either 'library' or 'program' to discern how to extract the info.",
        ),
        "enabled": attr.bool(default = False, doc = "Flag to enable/disable separate debug generation."),
        "deps": attr.label_list(providers = [CcInfo]),
        "cc_shared_library": attr.label(
            doc = "If extracting from a shared library, the target of the cc_shared_library. Otherwise empty.",
            allow_files = True,
        ),
        "_cc_toolchain": attr.label(default = "@bazel_tools//tools/cpp:current_cc_toolchain"),
        "_linux_constraint": attr.label(default = "@platforms//os:linux"),
        "_macos_constraint": attr.label(default = "@platforms//os:macos"),
        "_windows_constraint": attr.label(default = "@platforms//os:windows"),
    },
    doc = "Extract debuginfo into a separate file",
    toolchains = ["@bazel_tools//tools/cpp:toolchain_type"],
    fragments = ["cpp"],
    executable = True,
)
