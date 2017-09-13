#!/usr/bin/env python
"""
This script generates the compile expansions file used by Evergreen as part of the push/release
process.

Invoke by specifying an output file.
$ python generate_compile_expansions.py --out compile_expansions.yml
"""

import argparse
import json
import os
import re
import sys
import yaml

version_json = "version.json"


def generate_expansions():
    """Entry point for the script.

    This calls functions to generate version and scons cache expansions and
    writes them to a file.
    """
    args = parse_args()
    expansions = {}
    expansions.update(generate_version_expansions(args))
    expansions.update(generate_scons_cache_expansions())

    with open(args.out, "w") as out:
        print("saving compile expansions to {0}: ({1})".format(args.out, expansions))
        yaml.safe_dump(expansions, out, default_flow_style=False)


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True)
    return parser.parse_args()


def generate_version_expansions(args):
    """Generate expansions from a version.json file if given, or $MONGO_VERSION."""
    expansions = {}

    if os.path.exists(version_json):
        with open(version_json, "r") as f:
            data = f.read()
            version_data = json.loads(data)
        version_line = version_data['version']
        version_parts = match_verstr(version_line)
        if not version_parts:
            raise ValueError("Unable to parse version.json")
    else:
        if not os.getenv("MONGO_VERSION"):
            raise Exception("$MONGO_VERSION not set and no version.json provided")
        version_line = os.getenv("MONGO_VERSION").lstrip("r")
        version_parts = match_verstr(version_line)
        if not version_parts:
            raise ValueError("Unable to parse version from stdin and no version.json provided")

    if version_parts[0]:
        expansions["suffix"] = "latest"
        expansions["src_suffix"] = "latest"
    else:
        expansions["suffix"] = version_line
        expansions["src_suffix"] = "r{0}".format(version_line)
    expansions["version"] = version_line

    return expansions


def generate_scons_cache_expansions():
    """Generate scons cache expansions from some files and environment variables."""
    expansions = {}
    if sys.platform.startswith("win"):
        system_id_path = r"c:\mongodb-build-system-id"
        default_cache_path_base = r"z:\data\scons-cache"
    else:
        system_id_path = "/etc/mongodb-build-system-id"
        default_cache_path_base = "/data/scons-cache"

    if os.path.isfile(system_id_path):
        with open(system_id_path, "r") as f:
            default_cache_path = os.path.join(default_cache_path_base, f.readline().strip())

            expansions["scons_cache_path"] = default_cache_path

            scons_cache_mode = os.getenv("SCONS_CACHE_MODE")

            if scons_cache_mode in (None, ""):
                scons_cache_mode = "nolinked"

            if os.getenv("USE_SCONS_CACHE") not in (None, False, "false", ""):
                expansions["scons_cache_args"] = "--cache={0} --cache-dir='{1}'".format(
                    scons_cache_mode, default_cache_path)
    return expansions


def match_verstr(verstr):
    """
    This function matches a version string and captures the "extra" part.

    If the version is a release like "2.3.4" or "2.3.4-rc0", this will return
    None. If the version is a pre-release like "2.3.4-325-githash" or
    "2.3.4-pre-", this will return "-pre-" or "-325-githash" If the version
    begins with the letter 'r', it will also match, e.g. r2.3.4, r2.3.4-rc0,
    r2.3.4-git234, r2.3.4-rc0-234-githash If the version is invalid (i.e.
    doesn't start with "2.3.4" or "2.3.4-rc0", this will return False.
    """
    res = re.match(r'^r?(?:\d+\.\d+\.\d+(?:-rc\d+)?)(-.*)?', verstr)
    if not res:
        return False
    return res.groups()


if __name__ == "__main__":
    generate_expansions()
