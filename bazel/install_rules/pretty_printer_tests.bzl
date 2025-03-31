load("@bazel_tools//tools/cpp:toolchain_utils.bzl", "find_cpp_toolchain")
load("//bazel/install_rules:providers.bzl", "TestBinaryInfo")

# This will not currently work under bazel test/run until we have a version of gdb to use in bazel
def mongo_pretty_printer_test_impl(ctx):
    split_name = ctx.label.name.split("-")

    # eg optimizer_gdb_test
    short_name = split_name[-1]

    final_output_directory = "bazel-bin/install/bin/"

    runnable_binary = None
    for file in ctx.attr.test_binary.files.to_list():
        if file.extension == "" or file.extension == "exe":
            runnable_binary = file

    pretty_printer_directory = "pretty-printer"
    if "stripped" in split_name:
        pretty_printer_directory += "-stripped"
    pretty_printer_directory += "/"

    script_output = ctx.actions.declare_file(pretty_printer_directory + short_name + ".py")
    launcher_output = ctx.actions.declare_file(pretty_printer_directory + "pretty_printer_test_launcher_" + short_name + ".py")

    python = ctx.toolchains["@bazel_tools//tools/python:toolchain_type"].py3_runtime

    inputs = depset(transitive = [
        ctx.attr._pretty_printer_creation_script.files,
        ctx.attr.test_script.files,
        ctx.attr._pip_requirements_script.files,
        ctx.attr._pretty_printer_launcher_infile.files,
        python.files,
    ])
    outputs = [launcher_output, script_output]

    ctx.actions.run(
        executable = python.interpreter.path,
        outputs = outputs,
        inputs = inputs,
        arguments = [
            ctx.file._pretty_printer_creation_script.path,
            "--gdb-test-script=" + ctx.file.test_script.path,
            "--pip-requirements-script=" + ctx.file._pip_requirements_script.path,
            "--pretty-printer-output=" + script_output.path,
            "--pretty-printer-launcher-infile=" + ctx.file._pretty_printer_launcher_infile.path,
            # TODO have a way to get to gdb from inside bazel
            "--gdb-path=" + "/opt/mongodbtoolchain/v4/bin/gdb",
            # This is due to us being dependent on the final location of installed binaries - ideally we don't do this and run the tests
            # in place and not from another directory
            "--final-binary-path=" + final_output_directory + runnable_binary.basename,
            "--final-pretty-printer-path=" + final_output_directory + script_output.basename,
            "--pretty-printer-launcher-output=" + launcher_output.path,
        ],
        mnemonic = "MongoPrettyPrinterTestCreation",
    )

    default_provider = DefaultInfo(executable = launcher_output, files = depset(outputs))
    test_binary_provider = TestBinaryInfo(test_binaries = depset([launcher_output]))
    return [default_provider, test_binary_provider]

mongo_pretty_printer_test = rule(
    mongo_pretty_printer_test_impl,
    attrs = {
        "test_script": attr.label(allow_single_file = True),
        "test_binary": attr.label(),
        # TODO have a way to get to gdb from inside bazel
        #"_gdb": attr.label(allow_single_file = True, default = "//:gdb"),
        "_pretty_printer_creation_script": attr.label(allow_single_file = True, default = "//bazel/install_rules:pretty_printer_test_creator.py"),
        "_pip_requirements_script": attr.label(allow_single_file = True, default = "//buildscripts:pip_requirements.py"),
        "_pretty_printer_launcher_infile": attr.label(allow_single_file = True, default = "//src/mongo/util:pretty_printer_test_launcher.py.in"),
    },
    doc = "Create pretty printer tests",
    toolchains = ["@bazel_tools//tools/python:toolchain_type"],
    executable = True,
    test = True,
)
