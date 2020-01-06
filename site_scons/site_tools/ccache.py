# Copyright 2019 MongoDB Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import math
import os
import re
import subprocess

import SCons
from pkg_resources import parse_version

# This is the oldest version of ccache that offers support for -gsplit-dwarf
_ccache_version_min = parse_version("3.2.3")
_ccache_version_found = None


def exists(env):
    """Look for a viable ccache implementation that meets our version requirements."""

    # If we already generated, we definitely exist
    if "CCACHE_VERSION" in env:
        return True

    ccache = env.get("CCACHE", False)
    if not ccache:
        return False

    ccache = env.WhereIs(ccache)
    if not ccache:
        return False

    pipe = SCons.Action._subproc(
        env,
        SCons.Util.CLVar(ccache) + ["--version"],
        stdin="devnull",
        stderr="devnull",
        stdout=subprocess.PIPE,
    )

    if pipe.wait() != 0:
        return False

    validated = False
    for line in pipe.stdout:
        line = line.decode("utf-8")
        if validated:
            continue  # consume all data
        version_banner = re.search(r"^ccache version", line)
        if not version_banner:
            continue
        ccache_version = re.split("ccache version (.+)", line)
        if len(ccache_version) < 2:
            continue
        global _ccache_version_found
        _ccache_version_found = parse_version(ccache_version[1])
        if _ccache_version_found >= _ccache_version_min:
            validated = True

    return validated


def generate(env):
    """Add ccache support."""

    # If we have already generated the tool, don't generate it again.
    if "CCACHE_VERSION" in env:
        return

    # If we can't find ccache, or it is too old a version, don't
    # generate.
    if not exists(env):
        return

    # Record our found CCACHE_VERSION. Other tools that need to know
    # about ccache (like iecc) should query this variable to determine
    # if ccache is active. Looking at the CCACHE variable in the
    # environment is not sufficient, since the user may have set it,
    # but it doesn't work or is out of date.
    env["CCACHE_VERSION"] = _ccache_version_found

    # ccache does not support response files so force scons to always
    # use the full command
    #
    # Note: This only works for Python versions >= 3.5
    env["MAXLINELENGTH"] = math.inf

    # Add ccache to the relevant command lines. Wrap the reference to
    # ccache in the $( $) pattern so that turning ccache on or off
    # doesn't invalidate your build.
    env["CCCOM"] = "$( $CCACHE $)" + env["CCCOM"]
    env["CXXCOM"] = "$( $CCACHE $)" + env["CXXCOM"]
    env["SHCCCOM"] = "$( $CCACHE $)" + env["SHCCCOM"]
    env["SHCXXCOM"] = "$( $CCACHE $)" + env["SHCXXCOM"]
