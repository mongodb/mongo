#!/usr/bin/python

import os
import subprocess
import sys

SCONS_VERSION = os.environ.get('SCONS_VERSION', "2.5.0")

mongodb_root = os.path.dirname(os.path.dirname(__file__))
scons_dir = os.path.join(mongodb_root, 'src', 'third_party', 'scons-' + SCONS_VERSION)

if sys.platform == 'win32':
    args = [sys.executable, os.path.join(scons_dir, 'scons.py')] + sys.argv[1:]
    sys.exit(subprocess.call(args))
else:
    os.execv(os.path.join(scons_dir, 'scons.py'), sys.argv)
