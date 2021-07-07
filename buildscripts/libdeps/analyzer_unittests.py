#!/usr/bin/env python3
#
# Copyright 2021 MongoDB Inc.
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
"""Unittests for the graph analyzer."""

import json
import sys
import unittest

import networkx

import libdeps.analyzer
from libdeps.graph import LibdepsGraph, EdgeProps, NodeProps, CountTypes


def add_node(graph, node, builder):
    """Add a node to the graph."""

    graph.add_nodes_from([(node, {NodeProps.bin_type.name: builder})])


def add_edge(graph, from_node, to_node, **kwargs):
    """Add an edge to the graph."""

    edge_props = {
        EdgeProps.direct.name: kwargs[EdgeProps.direct.name],
        EdgeProps.visibility.name: int(kwargs[EdgeProps.visibility.name]),
    }

    graph.add_edges_from([(from_node, to_node, edge_props)])


def get_double_diamond_mock_graph():
    """Construct a mock graph which covers a double diamond structure."""

    graph = LibdepsGraph()
    graph.graph['build_dir'] = '.'
    graph.graph['graph_schema_version'] = 2
    graph.graph['deptypes'] = '''{
        "Global": 0,
        "Public": 1,
        "Private": 2,
        "Interface": 3,
    }'''

    # builds a graph of mostly public edges that looks like this:
    #
    #
    #                  /lib3.so               /lib7.so
    #                 |       \              |       \
    # <-lib1.so--lib2.so       lib5.so--lib6.so       lib9.so
    #                 |       /              |       /
    #                  \lib4.so               \lib8.so
    #

    add_node(graph, 'lib1.so', 'SharedLibrary')
    add_node(graph, 'lib2.so', 'SharedLibrary')
    add_node(graph, 'lib3.so', 'SharedLibrary')
    add_node(graph, 'lib4.so', 'SharedLibrary')
    add_node(graph, 'lib5.so', 'SharedLibrary')
    add_node(graph, 'lib6.so', 'SharedLibrary')
    add_node(graph, 'lib7.so', 'SharedLibrary')
    add_node(graph, 'lib8.so', 'SharedLibrary')
    add_node(graph, 'lib9.so', 'SharedLibrary')

    add_edge(graph, 'lib1.so', 'lib2.so', direct=True, visibility=graph.get_deptype('Public'))
    add_edge(graph, 'lib2.so', 'lib3.so', direct=True, visibility=graph.get_deptype('Public'))
    add_edge(graph, 'lib2.so', 'lib4.so', direct=True, visibility=graph.get_deptype('Public'))
    add_edge(graph, 'lib3.so', 'lib5.so', direct=True, visibility=graph.get_deptype('Public'))
    add_edge(graph, 'lib4.so', 'lib5.so', direct=True, visibility=graph.get_deptype('Public'))
    add_edge(graph, 'lib5.so', 'lib6.so', direct=True, visibility=graph.get_deptype('Public'))
    add_edge(graph, 'lib6.so', 'lib7.so', direct=True, visibility=graph.get_deptype('Public'))
    add_edge(graph, 'lib6.so', 'lib8.so', direct=True, visibility=graph.get_deptype('Public'))
    add_edge(graph, 'lib7.so', 'lib9.so', direct=True, visibility=graph.get_deptype('Public'))
    add_edge(graph, 'lib8.so', 'lib9.so', direct=True, visibility=graph.get_deptype('Public'))

    # trans for 3 and 4
    add_edge(graph, 'lib1.so', 'lib3.so', direct=False, visibility=graph.get_deptype('Public'))
    add_edge(graph, 'lib1.so', 'lib4.so', direct=False, visibility=graph.get_deptype('Public'))

    # trans for 5
    add_edge(graph, 'lib1.so', 'lib5.so', direct=False, visibility=graph.get_deptype('Public'))
    add_edge(graph, 'lib2.so', 'lib5.so', direct=False, visibility=graph.get_deptype('Public'))

    # trans for 6
    add_edge(graph, 'lib1.so', 'lib6.so', direct=False, visibility=graph.get_deptype('Public'))
    add_edge(graph, 'lib2.so', 'lib6.so', direct=False, visibility=graph.get_deptype('Public'))
    add_edge(graph, 'lib3.so', 'lib6.so', direct=False, visibility=graph.get_deptype('Public'))
    add_edge(graph, 'lib4.so', 'lib6.so', direct=False, visibility=graph.get_deptype('Public'))

    # trans for 7
    add_edge(graph, 'lib1.so', 'lib7.so', direct=False, visibility=graph.get_deptype('Public'))
    add_edge(graph, 'lib2.so', 'lib7.so', direct=False, visibility=graph.get_deptype('Public'))
    add_edge(graph, 'lib3.so', 'lib7.so', direct=False, visibility=graph.get_deptype('Public'))
    add_edge(graph, 'lib4.so', 'lib7.so', direct=False, visibility=graph.get_deptype('Public'))
    add_edge(graph, 'lib5.so', 'lib7.so', direct=False, visibility=graph.get_deptype('Public'))

    # trans for 8
    add_edge(graph, 'lib1.so', 'lib8.so', direct=False, visibility=graph.get_deptype('Public'))
    add_edge(graph, 'lib2.so', 'lib8.so', direct=False, visibility=graph.get_deptype('Public'))
    add_edge(graph, 'lib3.so', 'lib8.so', direct=False, visibility=graph.get_deptype('Public'))
    add_edge(graph, 'lib4.so', 'lib8.so', direct=False, visibility=graph.get_deptype('Public'))
    add_edge(graph, 'lib5.so', 'lib8.so', direct=False, visibility=graph.get_deptype('Public'))

    # trans for 9
    add_edge(graph, 'lib1.so', 'lib9.so', direct=False, visibility=graph.get_deptype('Public'))
    add_edge(graph, 'lib2.so', 'lib9.so', direct=False, visibility=graph.get_deptype('Public'))
    add_edge(graph, 'lib3.so', 'lib9.so', direct=False, visibility=graph.get_deptype('Public'))
    add_edge(graph, 'lib4.so', 'lib9.so', direct=False, visibility=graph.get_deptype('Public'))
    add_edge(graph, 'lib5.so', 'lib9.so', direct=False, visibility=graph.get_deptype('Public'))
    add_edge(graph, 'lib6.so', 'lib9.so', direct=False, visibility=graph.get_deptype('Public'))

    return graph


def get_basic_mock_graph():
    """Construct a mock graph which covers most cases and is easy to understand."""

    graph = LibdepsGraph()
    graph.graph['build_dir'] = '.'
    graph.graph['graph_schema_version'] = 2
    graph.graph['deptypes'] = '''{
        "Global": 0,
        "Public": 1,
        "Private": 2,
        "Interface": 3,
    }'''

    # builds a graph of mostly public edges:
    #
    #                         /-lib5.so
    #                  /lib3.so
    #                 |       \-lib6.so
    # <-lib1.so--lib2.so
    #                 |       /-lib5.so (private)
    #                  \lib4.so
    #                         \-lib6.so

    # nodes
    add_node(graph, 'lib1.so', 'SharedLibrary')
    add_node(graph, 'lib2.so', 'SharedLibrary')
    add_node(graph, 'lib3.so', 'SharedLibrary')
    add_node(graph, 'lib4.so', 'SharedLibrary')
    add_node(graph, 'lib5.so', 'SharedLibrary')
    add_node(graph, 'lib6.so', 'SharedLibrary')

    # direct edges
    add_edge(graph, 'lib1.so', 'lib2.so', direct=True, visibility=graph.get_deptype('Public'))
    add_edge(graph, 'lib2.so', 'lib3.so', direct=True, visibility=graph.get_deptype('Public'))
    add_edge(graph, 'lib2.so', 'lib4.so', direct=True, visibility=graph.get_deptype('Public'))
    add_edge(graph, 'lib4.so', 'lib6.so', direct=True, visibility=graph.get_deptype('Public'))
    add_edge(graph, 'lib3.so', 'lib5.so', direct=True, visibility=graph.get_deptype('Public'))
    add_edge(graph, 'lib3.so', 'lib6.so', direct=True, visibility=graph.get_deptype('Public'))
    add_edge(graph, 'lib4.so', 'lib5.so', direct=True, visibility=graph.get_deptype('Private'))

    # trans for 3
    add_edge(graph, 'lib1.so', 'lib3.so', direct=False, visibility=graph.get_deptype('Public'))

    # trans for 4
    add_edge(graph, 'lib1.so', 'lib4.so', direct=False, visibility=graph.get_deptype('Public'))

    # trans for 5
    add_edge(graph, 'lib2.so', 'lib5.so', direct=False, visibility=graph.get_deptype('Public'))
    add_edge(graph, 'lib1.so', 'lib5.so', direct=False, visibility=graph.get_deptype('Public'))

    # trans for 6
    add_edge(graph, 'lib2.so', 'lib6.so', direct=False, visibility=graph.get_deptype('Public'))
    add_edge(graph, 'lib1.so', 'lib6.so', direct=False, visibility=graph.get_deptype('Public'))

    return graph


class Tests(unittest.TestCase):
    """Common unittest for the libdeps graph analyzer module."""

    def run_analysis(self, expected, graph, algo, *args):
        """Check results of analysis generically."""

        analysis = [algo(graph, *args)]
        ga = libdeps.analyzer.LibdepsGraphAnalysis(analysis)
        printer = libdeps.analyzer.GaJsonPrinter(ga)
        result = json.loads(printer.get_json())
        self.assertEqual(result, expected)

    def run_counts(self, expected, graph):
        """Check results of counts generically."""

        analysis = libdeps.analyzer.counter_factory(
            graph,
            [name[0] for name in CountTypes.__members__.items() if name[0] != CountTypes.ALL.name])
        ga = libdeps.analyzer.LibdepsGraphAnalysis(analysis)
        printer = libdeps.analyzer.GaJsonPrinter(ga)
        result = json.loads(printer.get_json())
        self.assertEqual(result, expected)

    def test_graph_paths_basic(self):
        """Test for the GraphPaths analyzer on a basic graph."""

        libdeps_graph = LibdepsGraph(get_basic_mock_graph())

        expected_result = {
            "GRAPH_PATHS": {
                "('lib1.so', 'lib6.so')": [["lib1.so", "lib2.so", "lib3.so", "lib6.so"],
                                           ["lib1.so", "lib2.so", "lib4.so", "lib6.so"]]
            }
        }
        self.run_analysis(expected_result, libdeps_graph, libdeps.analyzer.GraphPaths, 'lib1.so',
                          'lib6.so')

        expected_result = {"GRAPH_PATHS": {"('lib4.so', 'lib5.so')": []}}
        self.run_analysis(expected_result, libdeps_graph, libdeps.analyzer.GraphPaths, 'lib4.so',
                          'lib5.so')

        expected_result = {
            "GRAPH_PATHS": {"('lib2.so', 'lib5.so')": [['lib2.so', 'lib3.so', 'lib5.so']]}
        }
        self.run_analysis(expected_result, libdeps_graph, libdeps.analyzer.GraphPaths, 'lib2.so',
                          'lib5.so')

    def test_graph_paths_double_diamond(self):
        """Test path algorithm on the double diamond graph."""

        libdeps_graph = LibdepsGraph(get_double_diamond_mock_graph())

        expected_result = {
            "GRAPH_PATHS": {
                "('lib1.so', 'lib9.so')":
                    [["lib1.so", "lib2.so", "lib3.so", "lib5.so", "lib6.so", "lib7.so", "lib9.so"],
                     ["lib1.so", "lib2.so", "lib3.so", "lib5.so", "lib6.so", "lib8.so", "lib9.so"],
                     ["lib1.so", "lib2.so", "lib4.so", "lib5.so", "lib6.so", "lib7.so", "lib9.so"],
                     ["lib1.so", "lib2.so", "lib4.so", "lib5.so", "lib6.so", "lib8.so", "lib9.so"]]
            }
        }
        self.run_analysis(expected_result, libdeps_graph, libdeps.analyzer.GraphPaths, 'lib1.so',
                          'lib9.so')

        expected_result = {
            "GRAPH_PATHS": {
                "('lib5.so', 'lib9.so')": [["lib5.so", "lib6.so", "lib7.so", "lib9.so"],
                                           ["lib5.so", "lib6.so", "lib8.so", "lib9.so"]]
            }
        }
        self.run_analysis(expected_result, libdeps_graph, libdeps.analyzer.GraphPaths, 'lib5.so',
                          'lib9.so')

        expected_result = {
            "GRAPH_PATHS": {
                "('lib2.so', 'lib6.so')": [["lib2.so", "lib3.so", "lib5.so", "lib6.so"],
                                           ["lib2.so", "lib4.so", "lib5.so", "lib6.so"]]
            }
        }
        self.run_analysis(expected_result, libdeps_graph, libdeps.analyzer.GraphPaths, 'lib2.so',
                          'lib6.so')

    def test_critical_paths_basic(self):
        """Test for the CriticalPaths for basic graph."""

        libdeps_graph = LibdepsGraph(get_basic_mock_graph())

        expected_result = {"CRITICAL_EDGES": {"('lib1.so', 'lib6.so')": [["lib1.so", "lib2.so"]]}}
        self.run_analysis(expected_result, libdeps_graph, libdeps.analyzer.CriticalEdges, 'lib1.so',
                          'lib6.so')

        expected_result = {"CRITICAL_EDGES": {"('lib1.so', 'lib5.so')": [["lib1.so", "lib2.so"]]}}
        self.run_analysis(expected_result, libdeps_graph, libdeps.analyzer.CriticalEdges, 'lib1.so',
                          'lib5.so')

    def test_critical_paths_double_diamond(self):
        """Test for the CriticalPaths for double diamond graph."""

        libdeps_graph = LibdepsGraph(get_double_diamond_mock_graph())

        expected_result = {"CRITICAL_EDGES": {"('lib1.so', 'lib9.so')": [["lib1.so", "lib2.so"]]}}
        self.run_analysis(expected_result, libdeps_graph, libdeps.analyzer.CriticalEdges, 'lib1.so',
                          'lib9.so')

        expected_result = {"CRITICAL_EDGES": {"('lib2.so', 'lib9.so')": [["lib5.so", "lib6.so"]]}}
        self.run_analysis(expected_result, libdeps_graph, libdeps.analyzer.CriticalEdges, 'lib2.so',
                          'lib9.so')

    def test_direct_depends_basic(self):
        """Test for the DirectDependents for basic graph."""

        libdeps_graph = LibdepsGraph(get_basic_mock_graph())

        expected_result = {"DIRECT_DEPENDS": {"lib6.so": ["lib3.so", "lib4.so"]}}
        self.run_analysis(expected_result, libdeps_graph, libdeps.analyzer.DirectDependents,
                          'lib6.so')

        expected_result = {'DIRECT_DEPENDS': {'lib1.so': []}}
        self.run_analysis(expected_result, libdeps_graph, libdeps.analyzer.DirectDependents,
                          'lib1.so')

    def test_direct_depends_double_diamond(self):
        """Test for the DirectDependents for double diamond graph."""

        libdeps_graph = LibdepsGraph(get_double_diamond_mock_graph())

        expected_result = {"DIRECT_DEPENDS": {"lib9.so": ["lib7.so", "lib8.so"]}}
        self.run_analysis(expected_result, libdeps_graph, libdeps.analyzer.DirectDependents,
                          'lib9.so')

        expected_result = {"DIRECT_DEPENDS": {"lib6.so": ["lib5.so"]}}
        self.run_analysis(expected_result, libdeps_graph, libdeps.analyzer.DirectDependents,
                          'lib6.so')

    def test_common_depends_basic(self):
        """Test for the CommonDependents for basic graph."""

        libdeps_graph = LibdepsGraph(get_basic_mock_graph())

        expected_result = {
            "COMMON_DEPENDS": {
                "('lib6.so', 'lib5.so')": ["lib1.so", "lib2.so", "lib3.so", "lib4.so"]
            }
        }
        self.run_analysis(expected_result, libdeps_graph, libdeps.analyzer.CommonDependents,
                          ['lib6.so', 'lib5.so'])

        expected_result = {
            "COMMON_DEPENDS": {
                "('lib5.so', 'lib6.so')": ["lib1.so", "lib2.so", "lib3.so", "lib4.so"]
            }
        }
        self.run_analysis(expected_result, libdeps_graph, libdeps.analyzer.CommonDependents,
                          ['lib5.so', 'lib6.so'])

        expected_result = {"COMMON_DEPENDS": {"('lib5.so', 'lib6.so', 'lib2.so')": ["lib1.so"]}}
        self.run_analysis(expected_result, libdeps_graph, libdeps.analyzer.CommonDependents,
                          ['lib5.so', 'lib6.so', 'lib2.so'])

    def test_common_depends_double_diamond(self):
        """Test for the CommonDependents for double diamond graph."""

        libdeps_graph = LibdepsGraph(get_double_diamond_mock_graph())

        expected_result = {
            "COMMON_DEPENDS": {
                "('lib9.so',)": [
                    "lib1.so", "lib2.so", "lib3.so", "lib4.so", "lib5.so", "lib6.so", "lib7.so",
                    "lib8.so"
                ]
            }
        }
        self.run_analysis(expected_result, libdeps_graph, libdeps.analyzer.CommonDependents,
                          ['lib9.so'])

        expected_result = {"COMMON_DEPENDS": {"('lib9.so', 'lib2.so')": ["lib1.so"]}}
        self.run_analysis(expected_result, libdeps_graph, libdeps.analyzer.CommonDependents,
                          ['lib9.so', 'lib2.so'])

        expected_result = {"COMMON_DEPENDS": {"('lib1.so', 'lib4.so', 'lib3.so')": []}}
        self.run_analysis(expected_result, libdeps_graph, libdeps.analyzer.CommonDependents,
                          ['lib1.so', 'lib4.so', 'lib3.so'])

    def test_exclude_depends_basic(self):
        """Test for the ExcludeDependents for basic graph."""

        libdeps_graph = LibdepsGraph(get_basic_mock_graph())

        expected_result = {"EXCLUDE_DEPENDS": {"('lib6.so', 'lib5.so')": []}}
        self.run_analysis(expected_result, libdeps_graph, libdeps.analyzer.ExcludeDependents,
                          ['lib6.so', 'lib5.so'])

        expected_result = {"EXCLUDE_DEPENDS": {"('lib3.so', 'lib1.so')": ["lib1.so", "lib2.so"]}}
        self.run_analysis(expected_result, libdeps_graph, libdeps.analyzer.ExcludeDependents,
                          ['lib3.so', 'lib1.so'])

        expected_result = {
            "EXCLUDE_DEPENDS": {
                "('lib6.so', 'lib1.so', 'lib2.so')": ["lib2.so", "lib3.so", "lib4.so"]
            }
        }
        self.run_analysis(expected_result, libdeps_graph, libdeps.analyzer.ExcludeDependents,
                          ['lib6.so', 'lib1.so', 'lib2.so'])

    def test_exclude_depends_double_diamond(self):
        """Test for the ExcludeDependents for double diamond graph."""

        libdeps_graph = LibdepsGraph(get_double_diamond_mock_graph())

        expected_result = {
            "EXCLUDE_DEPENDS": {"('lib6.so', 'lib4.so')": ["lib3.so", "lib4.so", "lib5.so"]}
        }
        self.run_analysis(expected_result, libdeps_graph, libdeps.analyzer.ExcludeDependents,
                          ['lib6.so', 'lib4.so'])

        expected_result = {"EXCLUDE_DEPENDS": {"('lib2.so', 'lib9.so')": []}}
        self.run_analysis(expected_result, libdeps_graph, libdeps.analyzer.ExcludeDependents,
                          ['lib2.so', 'lib9.so'])

        expected_result = {
            "EXCLUDE_DEPENDS": {
                "('lib8.so', 'lib1.so', 'lib2.so', 'lib3.so', 'lib4.so', 'lib5.so')": [
                    "lib5.so", "lib6.so"
                ]
            }
        }
        self.run_analysis(expected_result, libdeps_graph, libdeps.analyzer.ExcludeDependents,
                          ['lib8.so', 'lib1.so', 'lib2.so', 'lib3.so', 'lib4.so', 'lib5.so'])

    def test_counts_basic(self):
        """Test counts on basic graph."""

        libdeps_graph = LibdepsGraph(get_basic_mock_graph())

        expected_result = {
            "NODE": 6, "EDGE": 13, "DIR_EDGE": 7, "TRANS_EDGE": 6, "DIR_PUB_EDGE": 6,
            "PUB_EDGE": 12, "PRIV_EDGE": 1, "IF_EDGE": 0, "PROG": 0, "LIB": 6
        }
        self.run_counts(expected_result, libdeps_graph)

    def test_counts_double_diamond(self):
        """Test counts on double diamond graph."""

        libdeps_graph = LibdepsGraph(get_double_diamond_mock_graph())

        expected_result = {
            "NODE": 9, "EDGE": 34, "DIR_EDGE": 10, "TRANS_EDGE": 24, "DIR_PUB_EDGE": 10,
            "PUB_EDGE": 34, "PRIV_EDGE": 0, "IF_EDGE": 0, "PROG": 0, "LIB": 9
        }
        self.run_counts(expected_result, libdeps_graph)


if __name__ == '__main__':
    unittest.main()
