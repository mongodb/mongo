# Copyright (C) 2018 and later: Unicode, Inc. and others.
# License & terms of use: http://www.unicode.org/copyright.html

# Python 2/3 Compatibility (ICU-20299)
# TODO(ICU-20301): Remove this.
from __future__ import print_function

import sys

from . import *


def dir_for(file):
    if isinstance(file, LocalFile):
        return get_local_dirname(file.dirname)
    if isinstance(file, SrcFile):
        return "{SRC_DIR}"
    if isinstance(file, InFile):
        return "{IN_DIR}"
    if isinstance(file, TmpFile):
        return "{TMP_DIR}"
    if isinstance(file, OutFile):
        return "{OUT_DIR}"
    if isinstance(file, PkgFile):
        return "{PKG_DIR}"
    assert False


LOCAL_DIRNAME_SUBSTITUTIONS = {
    "SRC": "{SRC_DIR}",
    "FILTERS": "{FILTERS_DIR}",
    "CWD": "{CWD_DIR}"
}


def get_local_dirname(dirname):
    if dirname.startswith("/"):
        return dirname
    elif dirname.startswith("$"):
        # Note: directory separator substitution happens later
        sep_idx = dirname.find("/")
        if sep_idx == -1:
            sep_idx = len(dirname)
        variable = dirname[1:sep_idx]
        if variable in LOCAL_DIRNAME_SUBSTITUTIONS:
            return LOCAL_DIRNAME_SUBSTITUTIONS[variable] + dirname[sep_idx:]
    print(
        "Error: Local directory must be absolute, or relative to one of: " +
        (", ".join("$%s" % v for v in LOCAL_DIRNAME_SUBSTITUTIONS.keys())),
        file=sys.stderr
    )
    exit(1)


ALL_TREES = [
    "locales",
    "curr",
    "lang",
    "region",
    "zone",
    "unit",
    "coll",
    "brkitr",
    "rbnf",
]


def concat_dicts(*dicts):
    # There is not a super great way to do this in Python:
    new_dict = {}
    for dict in dicts:
        new_dict.update(dict)
    return new_dict


def repeated_execution_request_looper(request):
    # dictionary of lists to list of dictionaries:
    ld = [
        dict(zip(request.repeat_with, t))
        for t in zip(*request.repeat_with.values())
    ]
    if not ld:
        # No special options given in repeat_with
        ld = [{} for _ in range(len(request.input_files))]
    return zip(ld, request.specific_dep_files, request.input_files, request.output_files)


def format_single_request_command(request, cmd_template, common_vars):
    return cmd_template.format(
        ARGS = request.args.format(
            INPUT_FILES = [file.filename for file in request.input_files],
            OUTPUT_FILES = [file.filename for file in request.output_files],
            **concat_dicts(common_vars, request.format_with)
        )
    )


def format_repeated_request_command(request, cmd_template, loop_vars, common_vars):
    (iter_vars, _, input_file, output_file) = loop_vars
    return cmd_template.format(
        ARGS = request.args.format(
            INPUT_FILE = input_file.filename,
            OUTPUT_FILE = output_file.filename,
            **concat_dicts(common_vars, request.format_with, iter_vars)
        )
    )


def flatten_requests(requests, config, common_vars):
    result = []
    for request in requests:
        result += request.flatten(config, requests, common_vars)
    return result


def get_all_output_files(requests, include_tmp=False):
    files = []
    for request in requests:
        files += request.all_output_files()

    # Filter out all files but those in OUT_DIR if necessary.
    # It is also easy to filter for uniqueness; do it right now and return.
    if not include_tmp:
        files = (file for file in files if isinstance(file, OutFile))
        return list(set(files))

    # Filter for unique values.  NOTE: Cannot use set() because we need to accept same filename as
    # OutFile and TmpFile as different, and by default they evaluate as equal.
    return [f for _, f in set((type(f), f) for f in files)]


def compute_directories(requests):
    dirs = set()
    for file in get_all_output_files(requests, include_tmp=True):
        path = "%s/%s" % (dir_for(file), file.filename)
        dirs.add(path[:path.rfind("/")])
    return list(sorted(dirs))


class SpaceSeparatedList(list):
    """A list that joins itself with spaces when converted to a string."""
    def __str__(self):
        return " ".join(self)
