#!/usr/bin/env python2

from __future__ import print_function

import os
import sys

SCONS_VERSION = os.environ.get('SCONS_VERSION', "2.5.0")

mongodb_root = os.path.abspath(os.path.dirname(os.path.dirname(__file__)))
scons_dir = os.path.join(mongodb_root, 'src', 'third_party','scons-' + SCONS_VERSION,
                         'scons-local-' + SCONS_VERSION)

if not os.path.exists(scons_dir):
    print("Could not find SCons in '%s'" % (scons_dir))
    sys.exit(1)

sys.path = [scons_dir] + sys.path

try:
    import SCons.Script
except ImportError:
    print("Could not find SCons in '%s'" % (scons_dir))
    sys.exit(1)

SCons.Script.main()
