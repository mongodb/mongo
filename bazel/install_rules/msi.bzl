# Build an msi using the wix toolset.
# Building the msi involves running candle.exe -> light.exe -> msitrim.py
def mongo_msi_impl(ctx):
    candle_in = []
    candle_out = []
    candle_arguments = [ctx.attr._candle_wrapper_script.files.to_list()[0].path, ctx.executable._candle.path, "-wx"]

    # pass in version variables
    # eg. 8.1.0-alpha
    mongo_version = ctx.attr.mongo_version
    if mongo_version in ctx.var:
        mongo_version = ctx.var[mongo_version]

    # eg. 8.1
    mongo_major_version = ".".join(mongo_version.split(".")[:2])
    candle_arguments.append("-dMongoDBMajorVersion=" + mongo_major_version)

    # eg. 8.1.0
    mongo_no_revision_version = mongo_version.split("-")[0]
    candle_arguments.append("-dMongoDBVersion=" + mongo_no_revision_version)

    # pass in folder variables needed by wix
    for var, label in ctx.attr.deps.items():
        folder = ""
        for file in label.files.to_list():
            symlink = ctx.actions.declare_file(var + "/" + file.basename)
            ctx.actions.symlink(output = symlink, target_file = file)
            candle_in.append(symlink)
            folder = symlink.dirname
        candle_arguments.append("-d" + var + "=" + folder + "/")

    # pass in string variables needed by wix
    for var, value in ctx.attr.wix_vars.items():
        candle_arguments.append("-d" + var + "=" + value)

    # pass in custom action
    for file in ctx.attr.custom_action.files.to_list():
        if file.extension == "dll":
            candle_in.append(file)
            candle_arguments.append("-dCustomActionDll=" + file.path)

    # pass in merge module if needed
    if ctx.attr.use_merge_modules:
        for file in ctx.attr._merge_modules.files.to_list():
            if file.basename.find("CRT") > -1 and file.basename.find(ctx.attr.arch) > -1:
                candle_in.append(file)
                candle_arguments.append("-dMergeModulesBasePath=" + file.dirname)
                candle_arguments.append("-dMergeModuleFileCRT=" + file.basename)
                break

    # pass in architecture
    candle_arguments.append("-dPlatform=" + ctx.attr.arch)
    candle_arguments.append("-arch")
    candle_arguments.append(ctx.attr.arch)

    # pass in extension files needed by wix
    for extension in ctx.attr.extensions:
        candle_arguments.append("-ext")
        candle_arguments.append(extension)

    # pass in .wxs files
    output_directory = ""
    for label in ctx.attr.srcs:
        for file in label.files.to_list():
            candle_in.append(file)
            candle_arguments.append(file.path)

            # wix output files are the input files with wixobj extension instead
            output_file = ctx.actions.declare_file(file.basename.split(".")[0] + ".wixobj")
            candle_out.append(output_file)
            output_directory = output_file.dirname

    candle_arguments.append("-dOutDir=" + output_directory)
    candle_arguments.append("-dTargetDir=" + output_directory)
    candle_arguments.append("-out")
    candle_arguments.append(output_directory + "/")

    python = ctx.toolchains["@bazel_tools//tools/python:toolchain_type"].py3_runtime
    ctx.actions.run(
        outputs = candle_out,
        inputs = depset(transitive = [depset(candle_in), ctx.attr._candle_wrapper_script.files, python.files, ctx.attr._candle.files]),
        executable = python.interpreter.path,
        arguments = candle_arguments,
    )

    light_in = candle_out
    light_out = []
    light_arguments = ["-wx", "-cultures:null"]

    # pass in extension files needed by wix
    for extension in ctx.attr.extensions:
        light_arguments.append("-ext")
        light_arguments.append(extension)

    # internal consistency evaluators to skip
    for sice in ctx.attr.light_sice:
        light_arguments.append("-sice:" + sice)

    for file in candle_out:
        light_arguments.append(file.path)

    output_filename = ctx.label.name + "-" + mongo_version
    light_msi = ctx.actions.declare_file(output_filename + ".pre.msi")
    light_out.append(light_msi)
    light_arguments.append("-out")
    light_arguments.append(light_msi.path)

    ctx.actions.run(
        outputs = light_out,
        inputs = light_in,
        executable = ctx.executable._light,
        arguments = light_arguments,
    )

    output_msi = ctx.actions.declare_file(output_filename + ".msi")
    msi_trim_script = ctx.attr._msi_trim_script.files.to_list()[0].path

    ctx.actions.run(
        outputs = [output_msi],
        inputs = depset(transitive = [depset(light_out), ctx.attr._msi_trim_script.files, python.files]),
        executable = python.interpreter.path,
        arguments = [msi_trim_script, light_msi.path, output_msi.path],
    )

    return [DefaultInfo(
        files = depset([output_msi]),
    )]

mongo_msi = rule(
    mongo_msi_impl,
    attrs = {
        "srcs": attr.label_list(allow_files = [".wxs"]),
        "deps": attr.string_keyed_label_dict(allow_empty = True),
        "custom_action": attr.label(allow_files = [".dll"]),
        "extensions": attr.string_list(allow_empty = True),
        "mongo_version": attr.string(mandatory = True),
        "use_merge_modules": attr.bool(default = False),
        "wix_vars": attr.string_dict(allow_empty = True),
        "light_sice": attr.string_list(allow_empty = True),
        "arch": attr.string(default = "x64"),
        "_candle": attr.label(
            default = "@wix_toolset//:candle",
            allow_single_file = True,
            executable = True,
            cfg = "host",
        ),
        "_candle_wrapper_script": attr.label(
            doc = "The python msi trimming script to use.",
            default = "//buildscripts:candle_wrapper.py",
            allow_single_file = True,
        ),
        "_light": attr.label(
            default = "@wix_toolset//:light",
            allow_single_file = True,
            executable = True,
            cfg = "host",
        ),
        "_msi_trim_script": attr.label(
            doc = "The python msi trimming script to use.",
            default = "//buildscripts:msitrim.py",
            allow_single_file = True,
        ),
        "_merge_modules": attr.label(
            default = "@local_windows_msvc//:merge_modules",
        ),
    },
    doc = "Create a msi using wix toolset",
    toolchains = ["@bazel_tools//tools/python:toolchain_type"],
)
