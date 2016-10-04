"""Dagger allows SCons to track it's internal build dependency data for the
MongoDB project. The tool stores this information in a Graph object, which
is then exported to a pickle/JSON file once the build is complete.

This tool binds a method to the SCons Env, which can be executed by a call
to env.BuildDeps(filename)

To use this tool, add the following three lines to your SConstruct
file, after all environment configuration has been completed.

env.Tool("dagger")
dependencyDb = env.Alias("dagger", env.BuildDeps(desiredpathtostoregraph))
env.Requires(dependencyDb, desired alias)

The desired path determines where the graph object is stored (which
should be in the same directory as the accompanying command line tool)
The desired alias determines what you are tracking build dependencies for is
built before you try and extract the build dependency data.

To generate the graph, run the command "SCons dagger"
"""

# Copyright 2016 MongoDB Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
#     Unless required by applicable law or agreed to in writing, software
#     distributed under the License is distributed on an "AS IS" BASIS,
#     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#     See the License for the specific language governing permissions and
#     limitations under the License.

import logging
import subprocess
import sys

import SCons

import graph
import graph_consts


LIB_DB = [] # Stores every SCons library nodes
OBJ_DB = [] # Stores every SCons object file node
EXE_DB = {} # Stores every SCons executable node, with the object files that build into it {Executable: [object files]}


class DependencyCycleError(SCons.Errors.UserError):
    """Exception representing a cycle discovered in library dependencies."""

    def __init__(self, first_node):
        super(DependencyCycleError, self).__init__()
        self.cycle_nodes = [first_node]

    def __str__(self):
        return "Library dependency cycle detected: " + " => ".join(
            str(n) for n in self.cycle_nodes)


def list_process(items):
    """From WIL, converts lists generated from an NM command with unicode strings to lists
    with ascii strings
    """

    r = []
    for l in items:
        if isinstance(l, list):
            for i in l:
                if i.startswith('.L'):
                    continue
                else:
                    r.append(str(i))
        else:
            if l.startswith('.L'):
                continue
            else:
                r.append(str(l))
    return r


# TODO: Use the python library to read elf files,
# so we know the file exists at this point
def get_symbol_worker(object_file, task):
    """From WIL, launches a worker subprocess which collects either symbols defined
    or symbols required by an object file"""

    platform = 'linux' if sys.platform.startswith('linux') else 'darwin'

    if platform == 'linux':
        if task == 'used':
            cmd = r'nm "' + object_file + r'" | grep -e "U " | c++filt'
        elif task == 'defined':
            cmd = r'nm "' + object_file + r'" | grep -v -e "U " | c++filt'
    elif platform == 'darwin':
        if task == 'used':
            cmd = "nm -u " + object_file + " | c++filt"
        elif task == 'defined':
            cmd = "nm -jU " + object_file + " | c++filt"

    p = subprocess.Popen(cmd, shell=True, stdout=subprocess.PIPE)
    uses = p.communicate()[0].decode()

    if platform == 'linux':
        return list_process([use[19:] for use in uses.split('\n')
                             if use != ''])
    elif platform == 'darwin':
        return list_process([use.strip() for use in uses.split('\n')
                             if use != ''])


def emit_obj_db_entry(target, source, env):
    """Emitter for object files. We add each object file
    built into a global variable for later use"""

    for t in target:
        if str(t) is None:
            continue
        OBJ_DB.append(t)
    return target, source

def emit_prog_db_entry(target, source, env):
    for t in target:
        if str(t) is None:
            continue
        EXE_DB[t] = [str(s) for s in source]

    return target, source

def emit_lib_db_entry(target, source, env):
    """Emitter for libraries. We add each library
    into our global variable"""
    for t in target:
        if str(t) is None:
            continue
        LIB_DB.append(t)
    return target, source


def __compute_libdeps(node):
    """
    Computes the direct library dependencies for a given SCons library node.
    the attribute that it uses is populated by the Libdeps.py script
    """

    if getattr(node.attributes, 'libdeps_exploring', False):
        raise DependencyCycleError(node)

    env = node.get_env()
    deps = set()
    node.attributes.libdeps_exploring = True
    try:
        try:
            for child in env.Flatten(getattr(node.attributes, 'libdeps_direct',
                                             [])):
                if not child:
                    continue
                deps.add(child)

        except DependencyCycleError as e:
            if len(e.cycle_nodes) == 1 or e.cycle_nodes[0] != e.cycle_nodes[
                    -1]:
                e.cycle_nodes.insert(0, node)

            logging.error("Found a dependency cycle" + str(e.cycle_nodes))
    finally:
        node.attributes.libdeps_exploring = False

    return deps


def __generate_lib_rels(lib, g):
    """Generate all library to library dependencies, and determine
    for each library the object files it consists of."""

    lib_node = g.find_node(lib.get_path(), graph_consts.NODE_LIB)

    for child in __compute_libdeps(lib):
        if child is None:
            continue

        lib_dep = g.find_node(str(child), graph_consts.NODE_LIB)
        g.add_edge(graph_consts.LIB_LIB, lib_node.id, lib_dep.id)

    object_files = lib.all_children()
    for obj in object_files:
        object_path = str(obj)
        obj_node = g.find_node(object_path, graph_consts.NODE_FILE)
        obj_node.library = lib_node.id
        lib_node.add_defined_file(obj_node.id)


def __generate_sym_rels(obj, g):
    """Generate all to symbol dependency and definition location information
    """

    object_path = str(obj)
    file_node = g.find_node(object_path, graph_consts.NODE_FILE)

    symbols_used = get_symbol_worker(object_path, task="used")
    symbols_defined = get_symbol_worker(object_path, task="defined")

    for symbol in symbols_defined:
        symbol_node = g.find_node(symbol, graph_consts.NODE_SYM)
        symbol_node.add_library(file_node.library)
        symbol_node.add_file(file_node.id)
        file_node.add_defined_symbol(symbol_node.id)

        lib_node = g.get_node(file_node.library)
        if lib_node is not None:
            lib_node.add_defined_symbol(symbol_node.id)

    for symbol in symbols_used:
        symbol_node = g.find_node(symbol, graph_consts.NODE_SYM)
        g.add_edge(graph_consts.FIL_SYM, file_node.id, symbol_node.id)


def __generate_file_rels(obj, g):
    """Generate all file to file and by extension, file to library and library
    to file relationships
    """
    file_node = g.get_node(str(obj))

    if file_node is None:
        return

    if file_node.id not in g.get_edge_type(graph_consts.FIL_SYM):
        return

    for symbol in g.get_edge_type(graph_consts.FIL_SYM)[file_node.id]:
        symbol = g.get_node(symbol)
        objs = symbol.files
        if objs is None:
            continue

        for obj in objs:
            g.add_edge(graph_consts.FIL_FIL, file_node.id, obj)

def __generate_exe_rels(exe, g):
    """Generates all executable to library relationships, and populates the
    contained files field in each NodeExe object"""
    exe_node = g.find_node(str(exe), graph_consts.NODE_EXE)
    for lib in exe.all_children():
        lib = lib.get_path()
        if lib is None or not lib.endswith(".a"):
            continue
        lib_node = g.find_node(lib, graph_consts.NODE_LIB)
        g.add_edge(graph_consts.EXE_LIB, exe_node.id, lib_node.id)

    exe_node.contained_files = set(EXE_DB[exe])

def write_obj_db(target, source, env):
    """The bulk of the tool. This method takes all the objects and libraries
    which we have stored in the global LIB_DB and OBJ_DB variables and
    creates the build dependency graph. The graph is then exported to a JSON
    file for use with the separate query tool/visualizer
    """
    g = graph.Graph()

    for lib in LIB_DB:
        __generate_lib_rels(lib, g)

    for obj in OBJ_DB:
        __generate_sym_rels(obj, g)

    for obj in OBJ_DB:
        __generate_file_rels(obj, g)

    for exe in EXE_DB.keys():
        __generate_exe_rels(exe, g)

    # target is given as a list of target SCons nodes - this builder is only responsible for
    # building the json target, so this list is of length 1. export_to_json
    # expects a filename, whereas target is a list of SCons nodes so we cast target[0] to str

    g.export_to_json(str(target[0]))
