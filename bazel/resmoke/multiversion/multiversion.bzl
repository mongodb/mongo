"""multiversion_setup: download old MongoDB binaries via db-contrib-tool.

Example usage:

    load("//bazel/resmoke/multiversion:multiversion.bzl", "multiversion_setup")

    multiversion_setup(
        name = "7.0",
        version = "7.0",
    )

    resmoke_suite_test(
        name = "my_suite",
        config = ":my_suite.yml",
        multiversion_deps = [":7.0"],
        deps = ["//src/mongo/db:mongod"],
    )

The rule invokes:
    db-contrib-tool setup-repro-env <version> --edition <edition> --installDir <output_dir>/.install --linkDir <output_dir>

To reproduce a failure with pinned binaries, pass an Evergreen version ID, full git
commit hash, or Evergreen task ID via the per-target flag created by multiversion_setup:

    bazel test //my:suite \\
        --//bazel/resmoke/multiversion:last-continuous-pin=<evg-version-id>

The EVG version ID used by each run is recorded in multiversion-downloads.json inside
the multiversion directory, which is preserved in bazel-testlogs after each test run.
"""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo", "string_flag")

def _multiversion_setup_impl(ctx):
    output_dir = ctx.actions.declare_directory(ctx.label.name)

    evg_version = ctx.attr.evg_version_flag[BuildSettingInfo].value

    version_arg = (evg_version + "=" + ctx.attr.version) if evg_version else ctx.attr.version

    args = ctx.actions.args()
    args.add(ctx.executable._db_contrib_tool)
    args.add(version_arg)
    args.add(ctx.attr.edition)
    args.add(output_dir.path + "/.install")
    args.add(output_dir.path)
    args.add(output_dir.path + "/multiversion-downloads.json")
    args.add(ctx.executable._resmoke)
    args.add(ctx.file._mongo_version)

    ctx.actions.run(
        inputs = [ctx.file._mongo_version],
        outputs = [output_dir],
        executable = ctx.executable._wrapper,
        tools = [ctx.executable._db_contrib_tool, ctx.executable._resmoke],
        arguments = [args],
        execution_requirements = {
            # Binaries come from Evergreen and change as new builds are created;
            # never cache.
            "no-cache": "1",
            "no-remote-cache": "1",
            # db-contrib-tool reads outside its declared inputs during setup for
            # credential files so it cannot run in a hermetic sandbox or remotely.
            "no-sandbox": "1",
            "no-remote": "1",
        },
        mnemonic = "MultiversionSetup",
        progress_message = "Downloading multiversion binaries for %s" % ctx.label,
    )

    return [DefaultInfo(files = depset([output_dir]))]

_multiversion_setup_rule = rule(
    implementation = _multiversion_setup_impl,
    attrs = {
        "version": attr.string(
            mandatory = True,
            doc = "MongoDB version string passed to 'db-contrib-tool setup-repro-env'. Examples: '7.0', 'last-lts', 'last-continuous'.",
        ),
        "edition": attr.string(
            mandatory = True,
            doc = "MongoDB edition to download (e.g. 'enterprise', 'community'). Set automatically from the build configuration by the multiversion_setup macro.",
        ),
        "evg_version_flag": attr.label(
            mandatory = True,
            providers = [BuildSettingInfo],
            doc = "Label of the per-target string_flag for EVG version override. Set by the multiversion_setup macro; do not set directly.",
        ),
        "_db_contrib_tool": attr.label(
            executable = True,
            cfg = "exec",
            default = "@db_contrib_tool//:db-contrib-tool",
            doc = "db-contrib-tool binary.  Defaults to the repo-rule-downloaded binary.",
        ),
        "_resmoke": attr.label(
            executable = True,
            cfg = "exec",
            default = "//buildscripts:resmoke",
        ),
        "_mongo_version": attr.label(
            allow_single_file = True,
            default = "//bazel/resmoke:resmoke_mongo_version",
        ),
        "_wrapper": attr.label(
            executable = True,
            cfg = "exec",
            default = "//bazel/resmoke/multiversion:run_db_contrib_tool",
        ),
    },
    doc = """\
Downloads old MongoDB binaries using db-contrib-tool setup-repro-env.

The resulting directory target can be passed to resmoke_suite_test via the
multiversion_deps attribute, which wires it to resmoke's --multiversionDir flag.
""",
)

def _multiversion_exclude_tags_impl(ctx):
    exclude_tags_file = ctx.actions.declare_file(ctx.label.name + ".yml")

    multiversion_dir_args = " ".join([
        "--multiversionDir %s" % d.path
        for d in ctx.files.multiversion_setup
    ])

    cmd = """
_log=$(mktemp)
trap 'rm -f "$_log"' EXIT
if ! (
    set -e
    export GIT_DIR=$(dirname "$(readlink -f WORKSPACE.bazel)")/.git
    {resmoke} \
        --mongoVersionFile {version_file} \
        generate-multiversion-exclude-tags \
        --oldBinVersion {old_bin_version} \
        {multiversion_dirs} \
        --excludeTagsFilePath {output}
) > "$_log" 2>&1; then
    cat "$_log" >&2
    exit 1
fi
""".format(
        resmoke = ctx.executable._resmoke.path,
        version_file = ctx.file._mongo_version.path,
        old_bin_version = ctx.attr.old_bin_version,
        multiversion_dirs = multiversion_dir_args,
        output = exclude_tags_file.path,
    )

    ctx.actions.run_shell(
        inputs = ctx.files.multiversion_setup + [ctx.file._mongo_version, ctx.file._backports_required_file],
        outputs = [exclude_tags_file],
        tools = [ctx.executable._resmoke],
        command = cmd,
        execution_requirements = {
            "no-cache": "1",
            "no-remote-cache": "1",
            "no-sandbox": "1",
            "no-remote": "1",
        },
        mnemonic = "MultiversionExcludeTags",
        progress_message = "Generating multiversion exclude tags for %s" % ctx.label,
    )

    return [DefaultInfo(files = depset([exclude_tags_file]))]

_multiversion_exclude_tags = rule(
    implementation = _multiversion_exclude_tags_impl,
    attrs = {
        "old_bin_version": attr.string(
            mandatory = True,
            values = ["last_lts", "last_continuous"],
        ),
        "multiversion_setup": attr.label(
            mandatory = True,
        ),
        "_resmoke": attr.label(
            executable = True,
            cfg = "exec",
            default = "//buildscripts:resmoke",
        ),
        "_mongo_version": attr.label(
            allow_single_file = True,
            default = "//bazel/resmoke:resmoke_mongo_version",
        ),
        "_backports_required_file": attr.label(
            allow_single_file = True,
            default = "//etc:backports_required_for_multiversion_tests.yml",
        ),
    },
)

_VERSION_TO_OLD_BIN_VERSION = {
    "last-continuous": "last_continuous",
    "last-lts": "last_lts",
}

def multiversion_setup(name, version, **kwargs):
    """Downloads old MongoDB binaries and generates companion exclude-tags targets.

    Also creates a per-target string_flag <name>-pin that can be set on
    the bazel command line to pin downloads to a specific Evergreen version ID,
    full git commit hash, or Evergreen task ID:

        bazel test //my:suite \\
            --//bazel/resmoke/multiversion:last-continuous-pin=<evg-version-id>

    For last-continuous and last-lts, a <name>_exclude_tags companion is created
    by running 'resmoke.py generate-multiversion-exclude-tags'.  For other versions
    an empty no-op tag file is produced.  These are passed into resmoke_suite_test
    via --tagFile when the multiversion_setup target appears in multiversion_deps.
    """

    string_flag(
        name = name + "-pin",
        build_setting_default = "",
    )

    edition = select({
        "//bazel/config:build_enterprise_enabled": "enterprise",
        "//conditions:default": "targeted",
    })

    _multiversion_setup_rule(
        name = name,
        version = version,
        edition = edition,
        evg_version_flag = ":" + name + "-pin",
        **kwargs
    )

    old_bin_version = _VERSION_TO_OLD_BIN_VERSION.get(version)
    if old_bin_version:
        _multiversion_exclude_tags(
            name = name + "_exclude_tags",
            multiversion_setup = ":" + name,
            old_bin_version = old_bin_version,
        )
    else:
        native.genrule(
            name = name + "_exclude_tags",
            outs = [name + "_exclude_tags.yml"],
            cmd = "echo '' > $@",
        )
