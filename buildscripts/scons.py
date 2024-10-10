#!/usr/bin/env python3
"""Scons module."""

import os
import sys

SCONS_VERSION = os.environ.get("SCONS_VERSION", "3.1.2")

MONGODB_ROOT = os.path.abspath(os.path.dirname(os.path.dirname(__file__)))
SCONS_DIR = os.path.join(
    MONGODB_ROOT, "src", "third_party", "scons-" + SCONS_VERSION, "scons-local-" + SCONS_VERSION
)

if not os.path.exists(SCONS_DIR):
    print("Could not find SCons in '%s'" % (SCONS_DIR))
    sys.exit(1)

SITE_TOOLS_DIR = os.path.join(MONGODB_ROOT, "site_scons")

sys.path = [SCONS_DIR, SITE_TOOLS_DIR] + sys.path

# pylint: disable=C0413
from mongo.pip_requirements import MissingRequirements, verify_requirements

try:
    verify_requirements()
except MissingRequirements as ex:
    print(ex)
    sys.exit(1)

try:
    import SCons.Script
except ImportError as import_err:
    print("Could not import SCons from '%s'" % (SCONS_DIR))
    print("ImportError:", import_err)
    sys.exit(1)


def entrypoint():
    SCons.Script.main()


if __name__ == "__main__":
    entrypoint()
