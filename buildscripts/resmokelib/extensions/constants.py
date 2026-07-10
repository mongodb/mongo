"""Constants for testing extensions locally and in Evergreen."""

import os

cwd = os.getcwd()


# Path to the source YAML with extension options.
CONF_IN_PATH = os.path.join(
    cwd, "src", "mongo", "db", "extension", "test_examples", "configurations.yml"
)


# Directories to search for .so files in Evergreen and locally.
_test_srcdir = os.environ.get("TEST_SRCDIR")
if _test_srcdir:
    # In the Bazel test sandbox, extensions are available in runfiles at their workspace-relative
    # path.
    EVERGREEN_SEARCH_DIRS = [
        os.path.join(_test_srcdir, "_main", "install-extensions", "lib"),
    ]
    LOCAL_SEARCH_DIRS = [
        os.path.join(_test_srcdir, "_main", "install-extensions", "lib"),
    ]
else:
    EVERGREEN_SEARCH_DIRS = [os.path.join(cwd, "dist-test", "lib")]
    LOCAL_SEARCH_DIRS = [
        os.path.join(cwd, "bazel-bin", "install-dist-test", "lib"),
        os.path.join(cwd, "bazel-bin", "install-extensions", "lib"),
    ]

# Externally-published extensions: config and download cache (see download_external_extensions.py).
EXTERNAL_EXTENSIONS_CONF_PATH = os.path.join(cwd, "etc", "extensions.yml")
EXTERNAL_EXTENSIONS_CACHE_DIR = os.path.join(cwd, "build", "external-extensions")

# Path to test extensions signing public key.
TEST_PUBLIC_KEY_PATH = os.path.join(
    cwd,
    "src",
    "mongo",
    "db",
    "extension",
    "test_examples",
    "test_extensions_signing_keys",
    "test_extensions_signing_public_key.asc",
)
