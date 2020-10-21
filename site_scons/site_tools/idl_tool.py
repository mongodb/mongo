#!/usr/bin/env python3
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

# We lazily import this at generate time.
idlc = None

IDL_GLOBAL_DEPS = []


def idlc_emitter(target, source, env):
    """For each input IDL file, the tool produces a .cpp and .h file."""
    first_source = str(source[0])

    if not first_source.endswith(".idl"):
        raise ValueError(
            "Bad idl file name '%s', it must end with '.idl' " % (first_source)
        )

    base_file_name, _ = SCons.Util.splitext(str(target[0]))
    target_source = env.File(base_file_name + "_gen.cpp")
    target_header = env.File(base_file_name + "_gen.h")

    env.Alias("generated-sources", [target_source, target_header])

    return [target_source, target_header], source


IDLCAction = SCons.Action.Action("$IDLCCOM", "$IDLCCOMSTR")


def idl_scanner(node, env, path):

    # When generating ninja we only need to add the IDL_GLOBAL_DEPS
    # because the implicit dependencies will be picked up using the
    # deps=msvc method.
    if env.get("GENERATING_NINJA", False):
        return IDL_GLOBAL_DEPS

    nodes_deps_list = getattr(node.attributes, "IDL_NODE_DEPS", None)
    if nodes_deps_list is not None:
        return nodes_deps_list

    nodes_deps_list = IDL_GLOBAL_DEPS[:]

    with open(str(node), encoding="utf-8") as file_stream:
        parsed_doc = idlc.parser.parse(
            file_stream, str(node), idlc.CompilerImportResolver(["src"])
        )

    if not parsed_doc.errors and parsed_doc.spec.imports is not None:
        nodes_deps_list.extend(
            [env.File(d) for d in sorted(parsed_doc.spec.imports.dependencies)]
        )

    setattr(node.attributes, "IDL_NODE_DEPS", nodes_deps_list)
    return nodes_deps_list


idl_scanner = SCons.Scanner.Scanner(function=idl_scanner, skeys=[".idl"])

# TODO: create a scanner for imports when imports are implemented
IDLCBuilder = SCons.Builder.Builder(
    action=IDLCAction,
    emitter=idlc_emitter,
    src_suffix=".idl",
    suffix=".cpp",
    source_scanner=idl_scanner,
)


def generate(env):
    bld = IDLCBuilder

    env.Append(SCANNERS=idl_scanner)

    env["BUILDERS"]["Idlc"] = bld

    sys.path.append(env.Dir("#buildscripts").get_abspath())
    import buildscripts.idl.idl.compiler as idlc_mod

    global idlc
    idlc = idlc_mod

    env["IDLC"] = "$PYTHON buildscripts/idl/idlc.py"
    base_dir = env.Dir("$BUILD_DIR").path
    env["IDLCFLAGS"] = [
        "--include", "src",
        "--base_dir", base_dir,
        "--target_arch", "$TARGET_ARCH",
    ]
    env["IDLCCOM"] = "$IDLC $IDLCFLAGS --header ${TARGETS[1]} --output ${TARGETS[0]} $SOURCES"
    env["IDLCCOMSTR"] = ("Generating ${TARGETS[0]}"
        if not env.get("VERBOSE", "").lower() in ['true', '1']
        else None)
    env["IDLCSUFFIX"] = ".idl"

    IDL_GLOBAL_DEPS = env.Glob("#buildscripts/idl/*.py") + env.Glob(
        "#buildscripts/idl/idl/*.py"
    )
    env["IDL_HAS_INLINE_DEPENDENCIES"] = True


def exists(env):
    return True
