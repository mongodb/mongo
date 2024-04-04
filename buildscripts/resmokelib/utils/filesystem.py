"""Filesystem-related helper functions."""

import os


def build_hygienic_bin_path(parent=None, child=None):
    """Get the hygienic bin directory, optionally from `parent` and with `child`."""
    pjoin = os.path.join
    res = pjoin("dist-test", "bin")

    if parent:
        res = pjoin(parent, res)

    if child:
        res = pjoin(res, child)

    return res
