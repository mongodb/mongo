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
Libdeps Graph Analysis Tool.

This will perform various metric's gathering and linting on the
graph generated from SCons generate-libdeps-graph target. The graph
represents the dependency information between all binaries from the build.
"""

import sys

from enum import Enum, auto
from pathlib import Path

import networkx

sys.path.append(str(Path(__file__).parent.parent))
import scons  # pylint: disable=wrong-import-position

sys.path.append(str(Path(scons.MONGODB_ROOT).joinpath('site_scons')))
from libdeps_next import deptype  # pylint: disable=wrong-import-position


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


class DependsReportTypes(Enum):
    """Enums for the different type of depends reports to perform on a graph."""

    direct_depends = auto()
    common_depends = auto()
    exclude_depends = auto()


class EdgeProps(Enum):
    """Enums for edge properties."""

    direct = auto()
    visibility = auto()


class LibdepsGraph(networkx.DiGraph):
    """Class for analyzing the graph."""

    def __init__(self, graph=networkx.DiGraph()):
        """Load the graph data."""

        super().__init__(incoming_graph_data=graph)

        # Load in the graph and store a reversed version as well for quick look ups
        # the in directions.
        self.rgraph = graph.reverse()

    def number_of_edge_types(self, edge_type, value):
        """Count the graphs edges based on type."""

        return len([edge for edge in self.edges(data=True) if edge[2].get(edge_type) == value])

    def node_count(self):
        """Count the graphs nodes."""

        return self.number_of_nodes()

    def edge_count(self):
        """Count the graphs edges."""

        return self.number_of_edges()

    def direct_edge_count(self):
        """Count the graphs direct edges."""

        return self.number_of_edge_types(EdgeProps.direct.name, 1)

    def transitive_edge_count(self):
        """Count the graphs transitive edges."""

        return self.number_of_edge_types(EdgeProps.direct.name, 0)

    def direct_public_edge_count(self):
        """Count the graphs direct public edges."""

        return len([
            edge for edge in self.edges(data=True) if edge[2].get(EdgeProps.direct.name) == 1
            and edge[2].get(EdgeProps.visibility.name) == int(deptype.Public)
        ])

    def public_edge_count(self):
        """Count the graphs public edges."""

        return self.number_of_edge_types(EdgeProps.visibility.name, int(deptype.Public))

    def private_edge_count(self):
        """Count the graphs private edges."""

        return self.number_of_edge_types(EdgeProps.visibility.name, int(deptype.Private))

    def interface_edge_count(self):
        """Count the graphs interface edges."""

        return self.number_of_edge_types(EdgeProps.visibility.name, int(deptype.Interface))

    def direct_depends(self, node):
        """For given nodes, report what nodes depend directly on that node."""

        return [
            depender for depender in self[node]
            if self[node][depender].get(EdgeProps.direct.name) == 1
        ]

    def common_depends(self, nodes):
        """For a given set of nodes, report what nodes depend on all nodes from that set."""

        neighbor_sets = [set(self[node]) for node in nodes]
        return list(set.intersection(*neighbor_sets))

    def exclude_depends(self, nodes):
        """Find depends with exclusions.

        Given a node, and a set of other nodes, find what nodes depend on the given
        node, but do not depend on the set of nodes.
        """

        valid_depender_nodes = []
        for depender_node in set(self[nodes[0]]):
            if all([
                    bool(excludes_node not in set(self.rgraph[depender_node]))
                    for excludes_node in nodes[1:]
            ]):
                valid_depender_nodes.append(depender_node)
        return valid_depender_nodes


class LibdepsGraphAnalysis:
    """Runs the given analysis on the input graph."""

    def __init__(self, libdeps_graph, build_dir='build/opt', counts='all', depends_reports=None):
        """Perform analysis based off input args."""

        self.build_dir = Path(build_dir)
        self.libdeps_graph = libdeps_graph

        self.results = {}

        self.count_types = {
            CountTypes.node.name: ("num_nodes", libdeps_graph.node_count),
            CountTypes.edge.name: ("num_edges", libdeps_graph.edge_count),
            CountTypes.dir_edge.name: ("num_direct_edges", libdeps_graph.direct_edge_count),
            CountTypes.trans_edge.name: ("num_trans_edges", libdeps_graph.transitive_edge_count),
            CountTypes.dir_pub_edge.name: ("num_direct_public_edges",
                                           libdeps_graph.direct_public_edge_count),
            CountTypes.pub_edge.name: ("num_public_edges", libdeps_graph.public_edge_count),
            CountTypes.priv_edge.name: ("num_private_edges", libdeps_graph.private_edge_count),
            CountTypes.if_edge.name: ("num_interface_edges", libdeps_graph.interface_edge_count),
        }

        for name in DependsReportTypes.__members__.items():
            setattr(self, f'{name[0]}_key', name[0])

        if counts:
            self.run_graph_counters(counts)
        if depends_reports:
            self.run_depend_reports(depends_reports)

    def get_results(self):
        """Return the results fo the analysis."""

        return self.results

    def _strip_build_dir(self, node):
        """Small util function for making args match the graph paths."""

        node = Path(node)
        if str(node.absolute()).startswith(str(self.build_dir.absolute())):
            return str(node.relative_to(self.build_dir))
        else:
            raise Exception(
                f"build path not in node path: node: {node} build_dir: {self.build_dir}")

    def _strip_build_dirs(self, nodes):
        """Small util function for making a list of nodes match graph paths."""

        for node in nodes:
            yield self._strip_build_dir(node)

    def run_graph_counters(self, counts):
        """Run the various graph counters for nodes and edges."""

        for count_type in CountTypes.__members__.items():
            if count_type[0] in self.count_types:
                dict_name, func = self.count_types[count_type[0]]

                if count_type[0] in counts:
                    self.results[dict_name] = func()

    def run_depend_reports(self, depends_reports):
        """Run the various dependency reports."""

        if depends_reports.get(self.direct_depends_key):
            self.results[self.direct_depends_key] = {}
            for node in depends_reports[self.direct_depends_key]:
                self.results[self.direct_depends_key][node] = self.libdeps_graph.direct_depends(
                    self._strip_build_dir(node))

        if depends_reports.get(self.common_depends_key):
            self.results[self.common_depends_key] = {}
            for nodes in depends_reports[self.common_depends_key]:
                nodes = frozenset(self._strip_build_dirs(nodes))
                self.results[self.common_depends_key][nodes] = self.libdeps_graph.common_depends(
                    nodes)

        if depends_reports.get(self.exclude_depends_key):
            self.results[self.exclude_depends_key] = {}
            for nodes in depends_reports[self.exclude_depends_key]:
                nodes = tuple(self._strip_build_dirs(nodes))
                self.results[self.exclude_depends_key][nodes] = self.libdeps_graph.exclude_depends(
                    nodes)


class GaPrinter:
    """Base class for printers of the graph analysis."""

    def __init__(self, libdeps_graph_analysis):
        """Store the graph analysis for use when printing."""

        self.libdeps_graph_analysis = libdeps_graph_analysis


class GaJsonPrinter(GaPrinter):
    """Printer for json output."""

    def serialize(self, dictionary):
        """Serialize the k,v pairs in the dictionary."""

        new = {}
        for key, value in dictionary.items():
            if isinstance(value, dict):
                value = self.serialize(value)
            new[str(key)] = value
        return new

    def print(self):
        """Print the result data."""

        import json
        results = self.libdeps_graph_analysis.get_results()
        print(json.dumps(self.serialize(results)))


class GaPrettyPrinter(GaPrinter):
    """Printer for pretty console output."""

    count_desc = {
        CountTypes.node.name: ("num_nodes", "Nodes in Graph: {}"),
        CountTypes.edge.name: ("num_edges", "Edges in Graph: {}"),
        CountTypes.dir_edge.name: ("num_direct_edges", "Direct Edges in Graph: {}"),
        CountTypes.trans_edge.name: ("num_trans_edges", "Transitive Edges in Graph: {}"),
        CountTypes.dir_pub_edge.name: ("num_direct_public_edges",
                                       "Direct Public Edges in Graph: {}"),
        CountTypes.pub_edge.name: ("num_public_edges", "Public Edges in Graph: {}"),
        CountTypes.priv_edge.name: ("num_private_edges", "Private Edges in Graph: {}"),
        CountTypes.if_edge.name: ("num_interface_edges", "Interface Edges in Graph: {}"),
    }

    @staticmethod
    def _print_results_node_list(heading, nodes):
        """Util function for printing a list of nodes for depend reports."""

        print(heading)
        for i, depender in enumerate(nodes, start=1):
            print(f"\t{i}: {depender}")
        print("")

    def print(self):
        """Print the result data."""

        results = self.libdeps_graph_analysis.get_results()
        for count_type in CountTypes.__members__.items():
            if count_type[0] in self.count_desc:
                dict_name, desc = self.count_desc[count_type[0]]
                if dict_name in results:
                    print(desc.format(results[dict_name]))

        if DependsReportTypes.direct_depends.name in results:
            print("\nNodes that directly depend on:")
            for node in results[DependsReportTypes.direct_depends.name]:
                self._print_results_node_list(f'=>depends on {node}:',
                                              results[DependsReportTypes.direct_depends.name][node])

        if DependsReportTypes.common_depends.name in results:
            print("\nNodes that commonly depend on:")
            for nodes in results[DependsReportTypes.common_depends.name]:
                self._print_results_node_list(
                    f'=>depends on {nodes}:',
                    results[DependsReportTypes.common_depends.name][nodes])

        if DependsReportTypes.exclude_depends.name in results:
            print("\nNodes that depend on a node, but exclude others:")
            for nodes in results[DependsReportTypes.exclude_depends.name]:
                self._print_results_node_list(
                    f"=>depends: {nodes[0]}, exclude: {nodes[1:]}:",
                    results[DependsReportTypes.exclude_depends.name][nodes])
