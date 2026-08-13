"""Repository rule for importing Evergreen's libvoidstar into Bazel."""

_LIBVOIDSTAR_PATH_ENV = "LIBVOIDSTAR_PATH"
_DEFAULT_LIBVOIDSTAR_PATHS = [
    "/usr/lib/libvoidstar.so",
    "/usr/lib64/libvoidstar.so",
    "/usr/local/lib/libvoidstar.so",
]

_BUILD_FILE = """
package(default_visibility = ["//visibility:public"])

filegroup(
    name = "libvoidstar",
    srcs = ["libvoidstar.so"],
)
"""

def _find_libvoidstar(ctx):
    configured_path = ctx.os.environ.get(_LIBVOIDSTAR_PATH_ENV)
    candidates = [configured_path] if configured_path else _DEFAULT_LIBVOIDSTAR_PATHS
    for candidate in candidates:
        path = ctx.path(candidate)
        if path.exists:
            return path

    fail(
        (
            "libvoidstar was requested, but it was not found. Checked: {}. " +
            "Install libvoidstar or set --repo_env={}=/absolute/path/to/libvoidstar.so."
        ).format(", ".join(candidates), _LIBVOIDSTAR_PATH_ENV),
    )

def _setup_libvoidstar(ctx):
    source = _find_libvoidstar(ctx)
    copy = ctx.which("cp")
    if copy == None:
        fail("libvoidstar was found at {}, but cp is unavailable".format(source))
    result = ctx.execute([str(copy), str(source), str(ctx.path("libvoidstar.so"))])
    if result.return_code:
        fail(
            "could not import libvoidstar from {}: {}".format(
                source,
                result.stderr or result.stdout,
            ),
        )
    ctx.file("BUILD.bazel", _BUILD_FILE)

setup_libvoidstar = repository_rule(
    implementation = _setup_libvoidstar,
    configure = True,
    environ = [_LIBVOIDSTAR_PATH_ENV],
    local = True,
)
