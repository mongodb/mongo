load("@bazel_tools//tools/cpp:toolchain_utils.bzl", "find_cpp_toolchain")

TagInfo = provider(
    doc = "A rule provider to pass around tags that were passed to rules.",
    fields = {
        "tags": "Bazel tags that were attached to the rule.",
    },
)

WITH_DEBUG_SUFFIX = "_with_debug"
CC_SHARED_LIBRARY_SUFFIX = "_shared"
SHARED_ARCHIVE_SUFFIX = "_shared_archive"
MAC_DEBUG_FOLDER_EXTENSION = ".dSYM"

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
    input_files = ctx.attr.binary_with_debug.files.to_list()
    if len(input_files) == 0:
        return None, None, None, None
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
                if debug_ext == MAC_DEBUG_FOLDER_EXTENSION:
                    debug_info = ctx.actions.declare_directory(basename + shared_ext + debug_ext)
                else:
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
            if debug_ext == MAC_DEBUG_FOLDER_EXTENSION:
                debug_info = ctx.actions.declare_directory(basename + debug_ext)
            else:
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

def get_transitive_dyn_libs(deps):
    """
    Get a transitive list of all dynamic library files under a set of dependencies.
    """

    # TODO(SERVER-85819): Investigate to see if it's possible to merge the depset without looping over all transitive
    # dependencies.
    transitive_dyn_libs = []
    for dep in deps:
        for input in dep[CcInfo].linking_context.linker_inputs.to_list():
            for library in input.libraries:
                if library.dynamic_library:
                    transitive_dyn_libs.append(library.dynamic_library)
    return transitive_dyn_libs

def get_transitive_debug_files(deps):
    """
    Get a transitive list of all dynamic library files under a set of dependencies.
    """

    # TODO(SERVER-85819): Investigate to see if it's possible to merge the depset without looping over all transitive
    # dependencies.
    transitive_debugs = []
    for dep in deps:
        for input in dep[DefaultInfo].files.to_list():
            if input.basename.endswith(".debug"):
                transitive_debugs.append(input)
    return transitive_debugs

def symlink_shared_archive(ctx, shared_ext, static_ext):
    """
    Shared archives (.so.a/.dll.lib) have different extensions depending on the operating system.
    Strip the suffix added on by the target rule and replace the static library ext with the shared archive
    ext.
    """
    original_output = ctx.attr.shared_archive.files.to_list()[0]
    basename = original_output.basename[:-len(SHARED_ARCHIVE_SUFFIX + static_ext)]
    symlink = ctx.actions.declare_file(basename + shared_ext + static_ext)

    ctx.actions.symlink(
        output = symlink,
        target_file = original_output,
    )
    return symlink

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

        linker_input_deps = []
        for dep in ctx.attr.deps:
            linker_input_deps.append(dep[CcInfo].linking_context.linker_inputs)

        if shared_lib or static_lib:
            if shared_lib:
                so_path = shared_lib.path.replace(ctx.bin_dir.path + "/", "")
            else:
                so_path = ""
            direct_lib = cc_common.create_library_to_link(
                actions = ctx.actions,
                feature_configuration = feature_configuration,
                cc_toolchain = cc_toolchain,
                dynamic_library = shared_lib,
                dynamic_library_symlink_path = so_path,
                static_library = static_lib if cc_shared_library == None else None,
                alwayslink = True,
            )

            # For some reason Bazel isn't deduplicating the user link flags, which leads to them accumulating
            # with each layer added. Deduplicate them manually.
            #
            # This routine works by taking the current library's link args and searching for each of its dependency's link
            # args present contiguously. If a matching sub list is found, it's removed from the current link line as a duplicate.
            # This is to avoid removing positional arguments that may appear more than once.
            #
            # This solution may break in the case where a base dependency contains only one positional argument,
            # but this should never happen since we will always inject at least one non positional argument globally.
            cur_flags = ctx.attr.binary_with_debug[CcInfo].linking_context.linker_inputs.to_list()[0].user_link_flags
            for dep in ctx.attr.binary_with_debug[CcInfo].linking_context.linker_inputs.to_list()[1:]:
                for i in range(len(cur_flags)):
                    dep_flags = dep.user_link_flags
                    if dep_flags and cur_flags:
                        if cur_flags[i] == dep_flags[0] and cur_flags[i:i + len(dep_flags)] == dep_flags:
                            cur_flags = cur_flags[:i] + cur_flags[i + len(dep_flags):]
                            break

            linker_input = cc_common.create_linker_input(
                owner = ctx.label,
                libraries = depset(direct = [direct_lib]),
                user_link_flags = cur_flags,
            )
            linking_context = cc_common.create_linking_context(linker_inputs = depset(direct = [linker_input], transitive = linker_input_deps))

        else:
            linking_context = cc_common.create_linking_context(linker_inputs = depset(transitive = linker_input_deps))

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

    # CcInfo's linkopts are ignored by cc_shared_library by default. To support both transitive and nontransitive
    # dynamic linkopts use:
    #   cc_library's linkopts field for both static and dynamic transitive link opts
    #   cc_shared_library's user_link_flags field for dynamic non-transitive link opts
    all_user_link_flags = dict()
    for input in ctx.attr.binary_with_debug[CcInfo].linking_context.linker_inputs.to_list():
        for flag in input.user_link_flags:
            all_user_link_flags[flag] = True

    # We define global linkopts here too, remove duplicates to prevent repeats of the global opts
    # from accumulating.
    for flag in original_info.linker_input.user_link_flags:
        all_user_link_flags[flag] = True
    all_user_link_flags = [flag for flag, _ in all_user_link_flags.items()]

    if output_shared_lib:
        direct_lib = cc_common.create_library_to_link(
            actions = ctx.actions,
            feature_configuration = feature_configuration,
            cc_toolchain = cc_toolchain,
            # Replace reference to dynamic library with final name
            dynamic_library = output_shared_lib,
            dynamic_library_symlink_path = output_shared_lib.path.replace(ctx.bin_dir.path + "/", ""),
            # Omit reference to static library
        )
        linker_input = cc_common.create_linker_input(
            owner = ctx.label,
            libraries = depset(direct = [direct_lib], transitive = [depset(dep_libraries)]),
            user_link_flags = all_user_link_flags,
            additional_inputs = depset(original_info.linker_input.additional_inputs),
        )
    else:
        linker_input = cc_common.create_linker_input(
            owner = ctx.label,
            libraries = depset(transitive = [depset(dep_libraries)]),
            user_link_flags = all_user_link_flags,
            additional_inputs = depset(original_info.linker_input.additional_inputs),
        )

    return CcSharedLibraryInfo(
        dynamic_deps = original_info.dynamic_deps,
        exports = original_info.exports,
        link_once_static_libs = original_info.link_once_static_libs,
        linker_input = linker_input,
    )

# TODO(SERVER-101906): We assume the worst-case resource usage to avoid OOMs. Figure out if we can
# generalize numInputs for all link configurations so that this can more intelligently set resource
# expectations.
def linux_extract_resource_set(os, numInputs):
    return {
        "cpu": 6,
        "memory": 20 * 1024,  # 20 GB
        "local_test": 1,
    }

def linux_strip_resource_set(os, numInputs):
    return {
        "cpu": 3,
        "memory": 10 * 1024,  # 10 GB
        "local_test": 1,
    }

def linux_extraction(ctx, cc_toolchain, inputs):
    outputs = []
    unstripped_static_bin = None
    input_bin, output_bin, debug_info, static_lib = get_inputs_and_outputs(ctx, ".so", ".a", ".debug")
    input_file = ctx.attr.binary_with_debug.files.to_list()

    if input_bin:
        if ctx.attr.enabled:
            ctx.actions.run(
                executable = cc_toolchain.objcopy_executable,
                outputs = [debug_info],
                inputs = inputs,
                resource_set = linux_extract_resource_set if ctx.attr.type == "program" else None,
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
                resource_set = linux_strip_resource_set if ctx.attr.type == "program" else None,
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

    if len(input_file):
        if static_lib:
            unstripped_static_bin = propgate_static_lib(ctx, static_lib, ".a", inputs)
            outputs.append(unstripped_static_bin)

        if ctx.attr.shared_archive:
            unstripped_shared_archive = symlink_shared_archive(ctx, ".so", ".a")
            outputs.append(unstripped_shared_archive)

    # The final program binary depends on the existence of the dependent dynamic library files. With
    # build-without-the-bytes enabled, these aren't downloaded. Manually collect them and add them to the
    # output set.
    dynamic_deps_runfiles = ctx.runfiles(files = [])
    if ctx.attr.type == "program":
        dynamic_deps = get_transitive_dyn_libs(ctx.attr.deps)
        dynamic_deps_runfiles = ctx.attr.binary_with_debug[DefaultInfo].data_runfiles.merge(ctx.runfiles(files = get_transitive_dyn_libs(ctx.attr.deps)))
        outputs.extend(dynamic_deps)

    provided_info = [
        DefaultInfo(
            files = depset(outputs, transitive = [depset(get_transitive_debug_files(ctx.attr.deps))]),
            runfiles = dynamic_deps_runfiles,
            executable = output_bin if ctx.attr.type == "program" else None,
        ),
        create_new_ccinfo_library(ctx, cc_toolchain, output_bin, unstripped_static_bin, ctx.attr.cc_shared_library),
    ]

    if ctx.attr.type == "program":
        provided_info += [
            RunEnvironmentInfo(
                environment = ctx.attr.binary_with_debug[RunEnvironmentInfo].environment,
                inherited_environment = ctx.attr.binary_with_debug[RunEnvironmentInfo].inherited_environment,
            ),
        ]
        provided_info += [ctx.attr.binary_with_debug[DebugPackageInfo]]

    if ctx.attr.cc_shared_library != None:
        provided_info.append(
            create_new_cc_shared_library_info(ctx, cc_toolchain, output_bin, ctx.attr.cc_shared_library[CcSharedLibraryInfo], static_lib),
        )

    return provided_info

def macos_extraction(ctx, cc_toolchain, inputs):
    outputs = []
    unstripped_static_bin = None
    input_bin, output_bin, debug_info, static_lib = get_inputs_and_outputs(ctx, ".dylib", ".a", MAC_DEBUG_FOLDER_EXTENSION)
    input_file = ctx.attr.binary_with_debug.files.to_list()

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

    if len(input_file):
        if static_lib:
            unstripped_static_bin = propgate_static_lib(ctx, static_lib, ".a", inputs)
            outputs.append(unstripped_static_bin)

        if ctx.attr.shared_archive:
            unstripped_shared_archive = symlink_shared_archive(ctx, ".dylib", ".a")
            outputs.append(unstripped_shared_archive)

    # The final program binary depends on the existence of the dependent dynamic library files. With
    # build-without-the-bytes enabled, these aren't downloaded. Manually collect them and add them to the
    # output set.
    dynamic_deps_runfiles = ctx.runfiles(files = [])
    if ctx.attr.type == "program":
        dynamic_deps = get_transitive_dyn_libs(ctx.attr.deps)
        dynamic_deps_runfiles = ctx.attr.binary_with_debug[DefaultInfo].data_runfiles.merge(ctx.runfiles(files = get_transitive_dyn_libs(ctx.attr.deps)))
        outputs.extend(dynamic_deps)

    provided_info = [
        DefaultInfo(
            files = depset(outputs),
            executable = output_bin if ctx.attr.type == "program" else None,
            runfiles = ctx.attr.binary_with_debug[DefaultInfo].data_runfiles,
        ),
        create_new_ccinfo_library(ctx, cc_toolchain, output_bin, unstripped_static_bin, ctx.attr.cc_shared_library),
    ]

    if ctx.attr.type == "program":
        provided_info += [
            RunEnvironmentInfo(
                environment = ctx.attr.binary_with_debug[RunEnvironmentInfo].environment,
                inherited_environment = ctx.attr.binary_with_debug[RunEnvironmentInfo].inherited_environment,
            ),
        ]

    if ctx.attr.cc_shared_library != None:
        provided_info.append(
            create_new_cc_shared_library_info(ctx, cc_toolchain, output_bin, ctx.attr.cc_shared_library[CcSharedLibraryInfo]),
        )

    return provided_info

def windows_extraction(ctx, cc_toolchain, inputs):
    pdb = None
    if ctx.attr.type == "library":
        ext = ".lib"
        if ctx.attr.cc_shared_library and ctx.attr.enable_pdb:
            pdb = ctx.attr.cc_shared_library[OutputGroupInfo].pdb_file
    elif ctx.attr.type == "program":
        ext = ".exe"
        if ctx.attr.enable_pdb:
            pdb = ctx.attr.binary_with_debug[OutputGroupInfo].pdb_file
    else:
        fail("Can't extract debug info from unknown type: " + ctx.attr.type)

    input_file = ctx.attr.binary_with_debug.files.to_list()
    outputs = []
    output_library = None
    output_dynamic_library = None

    if len(input_file):
        basename = ctx.attr.binary_with_debug.files.to_list()[0].basename[:-len(WITH_DEBUG_SUFFIX + ext)]
        output = ctx.actions.declare_file(basename + ext)

        for input in ctx.attr.binary_with_debug.files.to_list():
            ext = "." + input.extension

            basename = input.basename[:-len(WITH_DEBUG_SUFFIX + ext)]
            output = ctx.actions.declare_file(basename + ext)
            outputs.append(output)

            if ext == ".lib":
                output_library = output

            ctx.actions.symlink(
                output = output,
                target_file = input,
            )

        if ctx.attr.cc_shared_library != None:
            for file in ctx.attr.cc_shared_library.files.to_list():
                if file.path.endswith(".dll"):
                    basename = file.basename[:-len(WITH_DEBUG_SUFFIX + CC_SHARED_LIBRARY_SUFFIX + ".dll")]
                    output = ctx.actions.declare_file(basename + ".dll")
                    outputs.append(output)

                    output_dynamic_library = output

                    ctx.actions.symlink(
                        output = output,
                        target_file = file,
                    )

        if pdb:
            if ctx.attr.cc_shared_library != None:
                basename = input.basename[:-len(WITH_DEBUG_SUFFIX + ".pdb")]
                pdb_output = ctx.actions.declare_file(basename + ".dll.pdb")
            else:
                basename = input.basename[:-len(WITH_DEBUG_SUFFIX + ext)]
                pdb_output = ctx.actions.declare_file(basename + ".pdb")
            outputs.append(pdb_output)

            ctx.actions.symlink(
                output = pdb_output,
                target_file = pdb.to_list()[0],
            )

        if ctx.attr.shared_archive:
            unstripped_shared_archive = symlink_shared_archive(ctx, ".dll", ".lib")
            outputs.append(unstripped_shared_archive)

    provided_info = [
        DefaultInfo(
            files = depset(outputs),
            executable = output if ctx.attr.type == "program" else None,
            runfiles = ctx.attr.binary_with_debug[DefaultInfo].data_runfiles,
        ),
        create_new_ccinfo_library(ctx, cc_toolchain, output_dynamic_library, output_library, ctx.attr.cc_shared_library),
    ]

    if ctx.attr.type == "program":
        provided_info += [
            RunEnvironmentInfo(
                environment = ctx.attr.binary_with_debug[RunEnvironmentInfo].environment,
                inherited_environment = ctx.attr.binary_with_debug[RunEnvironmentInfo].inherited_environment,
            ),
        ]

    if ctx.attr.cc_shared_library != None:
        provided_info.append(
            create_new_cc_shared_library_info(ctx, cc_toolchain, output_dynamic_library, ctx.attr.cc_shared_library[CcSharedLibraryInfo]),
        )

    return provided_info

def extract_debuginfo_impl(ctx):
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
        # When skipping the archives we have to skip modifying debug info
        # for the intermediates because we end up taking a dependency
        # on the _with_debug .a files
        if ctx.attr.skip_archive and ctx.attr.cc_shared_library == None:
            return_info = [ctx.attr.binary_with_debug[CcInfo]]
        else:
            return_info = linux_extraction(ctx, cc_toolchain, inputs)
    elif ctx.target_platform_has_constraint(macos_constraint):
        if ctx.attr.skip_archive and ctx.attr.cc_shared_library == None:
            return_info = [ctx.attr.binary_with_debug[CcInfo]]
        else:
            return_info = macos_extraction(ctx, cc_toolchain, inputs)
    elif ctx.target_platform_has_constraint(windows_constraint):
        return_info = windows_extraction(ctx, cc_toolchain, inputs)

    tag_provider = TagInfo(tags = ctx.attr.tags)
    return_info.append(tag_provider)
    return return_info

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
        "enable_pdb": attr.bool(default = False, doc = "Flag to enable pdb outputs on windows."),
        "deps": attr.label_list(providers = [CcInfo]),
        "cc_shared_library": attr.label(
            doc = "If extracting from a shared library, the target of the cc_shared_library. Otherwise empty.",
            allow_files = True,
        ),
        "shared_archive": attr.label(
            doc = "If generating a shared archive(.so.a/.dll.lib), the shared archive's cc_library. Otherwise empty.",
            allow_files = True,
        ),
        "skip_archive": attr.bool(default = False, doc = "Flag to skip generating archives."),
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
        "enable_pdb": attr.bool(default = False, doc = "Flag to enable pdb outputs on windows."),
        "deps": attr.label_list(providers = [CcInfo]),
        "cc_shared_library": attr.label(
            doc = "If extracting from a shared library, the target of the cc_shared_library. Otherwise empty.",
            allow_files = True,
        ),
        "shared_archive": attr.label(
            doc = "If generating a shared archive(.so.a/.dll.lib), the shared archive's cc_library. Otherwise empty.",
            allow_files = True,
        ),
        "skip_archive": attr.bool(default = False, doc = "Flag to skip generating archives."),
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

extract_debuginfo_test = rule(
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
        "enable_pdb": attr.bool(default = False, doc = "Flag to enable pdb outputs on windows."),
        "deps": attr.label_list(providers = [CcInfo]),
        "cc_shared_library": attr.label(
            doc = "If extracting from a shared library, the target of the cc_shared_library. Otherwise empty.",
            allow_files = True,
        ),
        "shared_archive": attr.label(
            doc = "If generating a shared archive(.so.a/.dll.lib), the shared archive's cc_library. Otherwise empty.",
            allow_files = True,
        ),
        "skip_archive": attr.bool(default = False, doc = "Flag to skip generating archives."),
        "_cc_toolchain": attr.label(default = "@bazel_tools//tools/cpp:current_cc_toolchain"),
        "_linux_constraint": attr.label(default = "@platforms//os:linux"),
        "_macos_constraint": attr.label(default = "@platforms//os:macos"),
        "_windows_constraint": attr.label(default = "@platforms//os:windows"),
    },
    doc = "Extract debuginfo into a separate file",
    toolchains = ["@bazel_tools//tools/cpp:toolchain_type"],
    fragments = ["cpp"],
    executable = True,
    test = True,
)
