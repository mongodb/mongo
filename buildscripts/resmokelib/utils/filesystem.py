"""Filesystem-related helper functions."""

import os
import tempfile


def mkdtemp_in_build_dir():
    """Use build/ as the temp directory since it's mapped to an EBS volume on Evergreen hosts."""
    build_tmp_dir = os.path.join("build", "tmp")
    os.makedirs(build_tmp_dir, exist_ok=True)

    return tempfile.mkdtemp(dir=build_tmp_dir)


def remove_if_exists(path):
    """Remove path if it exists."""
    try:
        os.remove(path)
    except OSError:
        pass


def is_yaml_file(filename: str) -> bool:
    """Return true if 'filename' ends in .yml or .yaml, and false otherwise."""
    return os.path.splitext(filename)[1] in (".yaml", ".yml")


def build_hygienic_bin_path(parent=None, child=None):
    """Get the hygienic bin directory, optionally from `parent` and with `child`."""
    pjoin = os.path.join
    res = pjoin("dist-test", "bin")

    if parent:
        res = pjoin(parent, res)

    if child:
        res = pjoin(res, child)

    return res
