#!/usr/bin/env python3
#
# Copyright 2020 MongoDB Inc.
#
# Permission is hereby granted, free of charge, to any person obtaining
# a copy of this software and associated documentation files (the
# "Software"), to deal in the Software without restriction, including
# without limitation the rights to use, copy, modify, merge, publish,
# distribute, sublicense, and/or sell copies of the Software, and to
# permit persons to whom the Software is furnished to do so, subject to
# the following conditions:
#
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
# KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
# WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
# LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
# OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
# WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#
"""
Libdeps Graph Enums.

These are used for attributing data across the build scripts and analyzer scripts.
"""
from enum import Enum, auto
from pathlib import Path
import json

import networkx

try:
    import progressbar
except ImportError:
    pass


# We need to disable invalid name here because it break backwards compatibility with
# our graph schemas. Possibly we could use lower case conversion process to maintain
# backwards compatibility and make pylint happy.
# pylint: disable=invalid-name
class CountTypes(Enum):
    """Enums for the different types of counts to perform on a graph."""

    ALL = auto()
    NODE = auto()
    EDGE = auto()
    DIR_EDGE = auto()
    TRANS_EDGE = auto()
    DIR_PUB_EDGE = auto()
    PUB_EDGE = auto()
    PRIV_EDGE = auto()
    IF_EDGE = auto()
    PROG = auto()
    LIB = auto()


class DependsReportTypes(Enum):
    """Enums for the different type of depends reports to perform on a graph."""

    DIRECT_DEPENDS = auto()
    COMMON_DEPENDS = auto()
    EXCLUDE_DEPENDS = auto()
    GRAPH_PATHS = auto()
    CRITICAL_EDGES = auto()
    IN_DEGREE_ONE = auto()
    SYMBOL_DEPENDS = auto()


class LinterTypes(Enum):
    """Enums for the different types of counts to perform on a graph."""

    ALL = auto()
    PUBLIC_UNUSED = auto()


class EdgeProps(Enum):
    """Enums for edge properties."""

    direct = auto()
    visibility = auto()
    symbols = auto()


class NodeProps(Enum):
    """Enums for node properties."""

    bin_type = auto()


def null_progressbar(items):
    """Fake stand-in for normal progressbar."""
    for item in items:
        yield item


class LibdepsGraph(networkx.DiGraph):
    """Class for analyzing the graph."""

    def __init__(self, graph=networkx.DiGraph()):
        """Load the graph data."""
        super().__init__(incoming_graph_data=graph)
        self._progressbar = None
        self._deptypes = None

    def get_deptype(self, deptype):
        """Convert graphs deptypes from json string to dict, and return requested value."""

        if not self._deptypes:
            self._deptypes = json.loads(self.graph.get('deptypes', "{}"))
            if self.graph['graph_schema_version'] == 1:
                # get and set the legacy values
                self._deptypes['Global'] = self._deptypes.get('Global', 0)
                self._deptypes['Public'] = self._deptypes.get('Public', 1)
                self._deptypes['Private'] = self._deptypes.get('Private', 2)
                self._deptypes['Interface'] = self._deptypes.get('Interface', 3)

        return self._deptypes[deptype]

    def get_direct_nonprivate_graph(self):
        """Get a graph view of direct nonprivate edges."""

        def filter_direct_nonprivate_edges(n1, n2):
            return (self[n1][n2].get(EdgeProps.direct.name) and
                    (self[n1][n2].get(EdgeProps.visibility.name) == self.get_deptype('Public') or
                     self[n1][n2].get(EdgeProps.visibility.name) == self.get_deptype('Interface')))

        return networkx.subgraph_view(self, filter_edge=filter_direct_nonprivate_edges)

    def get_node_tree(self, node):
        """Get a tree with the passed node as the single root."""

        direct_nonprivate_graph = self.get_direct_nonprivate_graph()
        substree_set = networkx.descendants(direct_nonprivate_graph, node)

        def subtree(n1):
            return n1 in substree_set or n1 == node

        return networkx.subgraph_view(direct_nonprivate_graph, filter_node=subtree)

    def get_progress(self, value=None):
        """
        Set if a progress bar should be used or not.

        No args means use progress bar if available.
        """

        if value is None:
            value = ('progressbar' in globals())

        if self._progressbar:
            return self._progressbar

        if value:

            def get_progress_bar(title, *args):
                custom_bar = progressbar.ProgressBar(widgets=[
                    title,
                    progressbar.Counter(format='[%(value)d/%(max_value)d]'),
                    progressbar.Timer(format=" Time: %(elapsed)s "),
                    progressbar.Bar(marker='>', fill=' ', left='|', right='|')
                ])
                return custom_bar(*args)

            self._progressbar = get_progress_bar
        else:
            self._progressbar = null_progressbar

        return self._progressbar


def load_libdeps_graph(graph_file):
    """Load a graphml file and create a LibdepGraph."""

    graph = networkx.read_graphml(graph_file)
    return LibdepsGraph(graph=graph)
