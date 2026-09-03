load("@bazel_skylib//lib:unittest.bzl", "asserts", "unittest")
load(":libvoidstar.bzl", "libvoidstar_missing_message")

def _missing_message_test_impl(ctx):
    env = unittest.begin(ctx)
    candidates = [
        "/tmp/libvoidstar;touch /tmp/marker",
        "/tmp/path with spaces/libvoidstar.so",
    ]

    asserts.equals(
        env,
        "libvoidstar was requested, but it was not found. Checked: " +
        "/tmp/libvoidstar;touch /tmp/marker, /tmp/path with spaces/libvoidstar.so. " +
        "Install libvoidstar or set --repo_env=LIBVOIDSTAR_PATH=/absolute/path/to/libvoidstar.so.\n",
        libvoidstar_missing_message(candidates),
    )
    return unittest.end(env)

_missing_message_test = unittest.make(_missing_message_test_impl)

def libvoidstar_test_suite(name):
    unittest.suite(
        name,
        _missing_message_test,
    )
