#!/usr/bin/env python2
# Copyright (C) 2017 MongoDB Inc.
#
# This program is free software: you can redistribute it and/or  modify
# it under the terms of the GNU Affero General Public License, version 3,
# as published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
"""IDL Compiler Scons Tool."""

import os.path
import subprocess
import sys

import SCons

def idlc_emitter(target, source, env):
    """For each input IDL file, the tool produces a .cpp and .h file."""
    first_source = str(source[0])

    if not first_source.endswith(".idl"):
        raise ValueError("Bad idl file name '%s', it must end with '.idl' " % (first_source))

    base_file_name, _ = SCons.Util.splitext(str(target[0]))
    target_source = base_file_name + "_gen.cpp"
    target_header = base_file_name + "_gen.h"

    env.Alias('generated-sources', [target_source, target_header])

    return [target_source, target_header], source


IDLCAction = SCons.Action.Action('$IDLCCOM', '$IDLCCOMSTR')


def idl_scanner(node, env, path):
    # Use the import scanner mode of the IDL compiler to file imported files
    cmd = [sys.executable, "buildscripts/idl/idlc.py",  '--include','src', str(node), '--write-dependencies']
    try:
        deps_str = subprocess.check_output(cmd)
    except subprocess.CalledProcessError as e:
        print("IDLC ERROR: %s" % (e.output) )
        raise

    deps_list = deps_str.splitlines()

    nodes_deps_list = [ env.File(d) for d in deps_list]
    nodes_deps_list.extend(env.Glob('#buildscripts/idl/*.py'))
    nodes_deps_list.extend(env.Glob('#buildscripts/idl/idl/*.py'))

    return nodes_deps_list


idl_scanner = SCons.Scanner.Scanner(function=idl_scanner, skeys=['.idl'])

# TODO: create a scanner for imports when imports are implemented
IDLCBuilder = SCons.Builder.Builder(
    action=IDLCAction,
    emitter=idlc_emitter,
    srcsuffx=".idl",
    suffix=".cpp",
    source_scanner = idl_scanner
    )


def generate(env):
    bld = IDLCBuilder

    env.Append(SCANNERS = idl_scanner)

    env['BUILDERS']['Idlc'] = bld

    env['IDLC'] = sys.executable + " buildscripts/idl/idlc.py"
    env['IDLCFLAGS'] = ''
    base_dir = env.subst('$BUILD_ROOT/$VARIANT_DIR').replace("#", "")
    env['IDLCCOM'] = '$IDLC --include src --base_dir %s --target_arch $TARGET_ARCH --header ${TARGETS[1]} --output ${TARGETS[0]} $SOURCES ' % (base_dir)
    env['IDLCSUFFIX'] = '.idl'

    env['IDL_HAS_INLINE_DEPENDENCIES'] = True

def exists(env):
    return True
