"""Rules for assembling directory artifacts without architecture-specific tools."""

def _relative_source_path(file, repository_names):
    """Return the path of an external file relative to its repository root."""
    for repository_name in repository_names:
        for prefix in [
            "../%s/" % repository_name,
            "external/%s/" % repository_name,
            "%s/" % repository_name,
        ]:
            if file.short_path.startswith(prefix):
                return file.short_path[len(prefix):]

    fail(
        "{} is not under one of the declared external repositories: {}".format(
            file.short_path,
            ", ".join(repository_names),
        ),
    )

def _copy_files_to_directory_impl(ctx):
    output_directory = ctx.actions.declare_directory(ctx.attr.out)
    args = ctx.actions.args()
    args.add(output_directory.path)

    for source in ctx.files.srcs:
        args.add(source.path)
        args.add(_relative_source_path(source, ctx.attr.repository_names))

    ctx.actions.run_shell(
        inputs = ctx.files.srcs,
        outputs = [output_directory],
        arguments = [args],
        command = """
set -eu

output_directory="$1"
shift
mkdir -p "$output_directory"

while [ "$#" -gt 0 ]; do
    source="$1"
    destination="$2"
    shift 2

    mkdir -p "$output_directory/$(dirname "$destination")"
    # -L matches copy_to_directory's behavior of resolving symlinks before
    # copying. This prevents the output tree from containing links whose
    # referents were not explicitly included in the inputs.
    cp -aL -- "$source" "$output_directory/$destination"
done
""",
        mnemonic = "CopyFilesToDirectory",
        progress_message = "Assembling %s" % output_directory.short_path,
    )

    return [DefaultInfo(files = depset([output_directory]))]

copy_files_to_directory = rule(
    implementation = _copy_files_to_directory_impl,
    attrs = {
        "srcs": attr.label_list(
            allow_files = True,
            mandatory = True,
            doc = "Files to copy into the output directory.",
        ),
        "out": attr.string(
            mandatory = True,
            doc = "Name of the output directory artifact.",
        ),
        "repository_names": attr.string_list(
            mandatory = True,
            doc = "External repository names whose roots should be stripped.",
        ),
    },
    doc = """Assemble files from external repositories into a directory artifact.

This intentionally uses the shell's native file operations instead of a
prebuilt copy_to_directory binary. That keeps the rule usable on Linux
architectures for which Aspect Bazel Lib does not publish that binary.
""",
)
