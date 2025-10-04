"""Constants for testing extensions locally and in Evergreen."""

import os
import tempfile

temp_dir = tempfile.gettempdir()

cwd = os.getcwd()

# Directory for generated extension .conf files.
CONF_OUT_DIR = os.path.join(temp_dir, "mongo", "extensions")

# Path to the source YAML with extension options.
CONF_IN_PATH = os.path.join(
    cwd, "src", "mongo", "db", "extension", "test_examples", "configurations.yml"
)

# Directories to search for .so files in Evergreen and locally.
EVERGREEN_SEARCH_DIRS = [os.path.join(cwd, "dist-test", "lib")]

LOCAL_SEARCH_DIRS = [
    os.path.join(cwd, "bazel-bin", "install-dist-test", "lib"),
    os.path.join(cwd, "bazel-bin", "install-extensions", "lib"),
]
