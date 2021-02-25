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

import networkx


class CountTypes(Enum):
    """Enums for the different types of counts to perform on a graph."""

    all = auto()
    node = auto()
    edge = auto()
    dir_edge = auto()
    trans_edge = auto()
    dir_pub_edge = auto()
    pub_edge = auto()
    priv_edge = auto()
    if_edge = auto()
    shim = auto()
    prog = auto()
    lib = auto()


class DependsReportTypes(Enum):
    """Enums for the different type of depends reports to perform on a graph."""

    direct_depends = auto()
    common_depends = auto()
    exclude_depends = auto()


class LinterTypes(Enum):
    """Enums for the different types of counts to perform on a graph."""

    all = auto()
    public_unused = auto()


class EdgeProps(Enum):
    """Enums for edge properties."""

    direct = auto()
    visibility = auto()
    symbols = auto()
    shim = auto()


class NodeProps(Enum):
    """Enums for node properties."""

    shim = auto()
    bin_type = auto()


class LibdepsGraph(networkx.DiGraph):
    """Class for analyzing the graph."""

    def __init__(self, graph=networkx.DiGraph()):
        """Load the graph data."""

        super().__init__(incoming_graph_data=graph)

        # Load in the graph and store a reversed version as well for quick look ups
        # the in directions.
        self.rgraph = graph.reverse()
