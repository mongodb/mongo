"""mongot_setup: download the mongot-localdev binaries via db-contrib-tool.

Example usage:

    load("//bazel/resmoke/mongot:mongot.bzl", "mongot_setup")

    mongot_setup(name = "mongot-localdev")

    resmoke_suite_test(
        name = "my_mongot_e2e_suite",
        config = ":my_suite.yml",
        data = ["//bazel/resmoke/mongot:mongot-localdev"],
        deps = ["//src/mongo/db:mongod"],
    )

The rule invokes:
    db-contrib-tool setup-mongot-repro-env <version> --platform <platform> \\
        --architecture <arch> --installDir <tmpdir>
and places the resulting mongot-localdev tree in the output directory, which
the resmoke shim links into the working directory so that resmoke's default
mongot path (mongot-localdev/mongot) resolves.

The mongot version can be selected on the command line:

    bazel test //my:suite --//bazel/resmoke/mongot:version=release

or, for downstream 10gen/mongot patches, a prebuilt tarball URL can be
supplied, which is downloaded instead of invoking db-contrib-tool:

    bazel test //my:suite --//bazel/resmoke/mongot:localdev-url=<url>
"""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")

def _mongot_setup_impl(ctx):
    output_dir = ctx.actions.declare_directory(ctx.label.name)

    args = ctx.actions.args()
    args.add(ctx.executable._db_contrib_tool)
    args.add(ctx.attr.version_flag[BuildSettingInfo].value)
    args.add(ctx.attr.url_flag[BuildSettingInfo].value)
    args.add(output_dir.path)

    ctx.actions.run(
        outputs = [output_dir],
        executable = ctx.executable._wrapper,
        tools = [ctx.executable._db_contrib_tool],
        arguments = [args],
        execution_requirements = {
            # "latest" tracks the HEAD of 10gen/mongot and changes as new
            # builds are created; never cache.
            "no-cache": "1",
            "no-remote-cache": "1",
            # The download requires network access and Evergreen credentials
            # on the invoking host, so it cannot run in a hermetic sandbox or
            # remotely. The resulting tree is a regular output artifact, so
            # downstream test actions can still execute remotely.
            "no-sandbox": "1",
            "no-remote": "1",
        },
        mnemonic = "MongotSetup",
        progress_message = "Downloading mongot binaries for %s" % ctx.label,
    )

    return [DefaultInfo(files = depset([output_dir]))]

mongot_setup = rule(
    implementation = _mongot_setup_impl,
    attrs = {
        "version_flag": attr.label(
            providers = [BuildSettingInfo],
            default = "//bazel/resmoke/mongot:version",
            doc = "string_flag selecting the mongot version ('latest' or 'release').",
        ),
        "url_flag": attr.label(
            providers = [BuildSettingInfo],
            default = "//bazel/resmoke/mongot:localdev-url",
            doc = "string_flag with a prebuilt mongot-localdev tarball URL. " +
                  "When set, the tarball is downloaded instead of invoking db-contrib-tool.",
        ),
        "_db_contrib_tool": attr.label(
            executable = True,
            cfg = "exec",
            default = "@db_contrib_tool//:db-contrib-tool",
            doc = "db-contrib-tool binary. Defaults to the repo-rule-downloaded binary.",
        ),
        "_wrapper": attr.label(
            executable = True,
            cfg = "exec",
            default = "//bazel/resmoke/mongot:setup_mongot",
        ),
    },
    doc = """\
Downloads the mongot-localdev binaries using db-contrib-tool
setup-mongot-repro-env (or a prebuilt tarball URL).

The resulting directory target can be passed to resmoke_suite_test via data;
the resmoke shim places it at mongot-localdev/ in the working directory where
resmoke expects it.
""",
)
