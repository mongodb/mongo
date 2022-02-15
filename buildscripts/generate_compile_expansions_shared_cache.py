#!/usr/bin/env python3
"""
Generate the compile expansions file used by Evergreen as part of the push/release process.

Invoke by specifying an output file.
$ python generate_compile_expansions.py --out compile_expansions.yml
"""

import argparse
import json
import os
import re
import sys
import shlex
import yaml

VERSION_JSON = "version.json"


def generate_expansions():
    """Entry point for the script.

    This calls functions to generate version and scons cache expansions and
    writes them to a file.
    """
    args = parse_args()
    expansions = {}
    expansions.update(generate_version_expansions())
    expansions.update(generate_scons_cache_expansions())

    with open(args.out, "w") as out:
        print("saving compile expansions to {0}: ({1})".format(args.out, expansions))
        yaml.safe_dump(expansions, out, default_flow_style=False)


def parse_args():
    """Parse program arguments."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True)
    return parser.parse_args()


def generate_version_expansions():
    """Generate expansions from a version.json file if given, or $MONGO_VERSION."""
    expansions = {}

    if os.path.exists(VERSION_JSON):
        with open(VERSION_JSON, "r") as fh:
            data = fh.read()
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
        expansions["suffix"] = "v5.3-latest"
        expansions["src_suffix"] = "v5.3-latest"
        expansions["is_release"] = "false"
    else:
        expansions["suffix"] = version_line
        expansions["src_suffix"] = "r{0}".format(version_line)
        expansions["is_release"] = "true"
    expansions["version"] = version_line

    return expansions


def generate_scons_cache_expansions():
    """Generate scons cache expansions from some files and environment variables."""
    expansions = {}

    # Get the scons cache mode
    scons_cache_mode = os.getenv("SCONS_CACHE_MODE", "nolinked")

    # Get the host uuid
    if sys.platform.startswith("win"):
        system_id_path = r"c:\mongodb-build-system-id"
    else:
        system_id_path = "/etc/mongodb-build-system-id"

    if os.path.isfile(system_id_path):
        with open(system_id_path, "r") as fh:
            system_uuid = fh.readline().strip()

    # Set the scons shared cache setting

    # Global shared cache using EFS
    if os.getenv("SCONS_CACHE_SCOPE") == "shared":
        if sys.platform.startswith("win"):
            shared_mount_root = 'X:\\'
        else:
            shared_mount_root = '/efs'
        default_cache_path = os.path.join(shared_mount_root, system_uuid, "scons-cache")
        expansions["scons_cache_path"] = default_cache_path
        expansions[
            "scons_cache_args"] = "--cache={0} --cache-signature-mode=validate --cache-dir={1}".format(
                scons_cache_mode, shlex.quote(default_cache_path))

    # Local shared cache - host-based
    elif os.getenv("SCONS_CACHE_SCOPE") == "local":

        if sys.platform.startswith("win"):
            default_cache_path_base = r"z:\data\scons-cache"
        else:
            default_cache_path_base = "/data/scons-cache"

        default_cache_path = os.path.join(default_cache_path_base, system_uuid)
        expansions["scons_cache_path"] = default_cache_path
        expansions[
            "scons_cache_args"] = "--cache={0} --cache-signature-mode=validate --cache-dir={1}".format(
                scons_cache_mode, shlex.quote(default_cache_path))
    # No cache
    else:
        # Anything else is 'none'
        print("No cache used")

    return expansions


def match_verstr(verstr):
    """Match a version string and capture the "extra" part.

    If the version is a release like "2.3.4" or "2.3.4-rc0", this will return
    None. If the version is a pre-release like "2.3.4-325-githash" or
    "2.3.4-pre-", this will return "-pre-" or "-325-githash" If the version
    begins with the letter 'r', it will also match, e.g. r2.3.4, r2.3.4-rc0,
    r2.3.4-git234, r2.3.4-rc0-234-githash If the version is invalid (i.e.
    doesn't start with "2.3.4" or "2.3.4-rc0", this will return False.
    """
    res = re.match(r'^r?(?:\d+\.\d+\.\d+(?:-rc\d+|-alpha\d+)?)(-.*)?', verstr)
    if not res:
        return False
    return res.groups()


if __name__ == "__main__":
    generate_expansions()
