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
from generate_test_graphs import get_double_diamond_mock_graph, get_basic_mock_graph


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

        expected_result = {"CRITICAL_EDGES": {"('lib5.so', 'lib6.so')": []}}
        self.run_analysis(expected_result, libdeps_graph, libdeps.analyzer.CriticalEdges, 'lib5.so',
                          'lib6.so')

    def test_critical_paths_double_diamond(self):
        """Test for the CriticalPaths for double diamond graph."""

        libdeps_graph = LibdepsGraph(get_double_diamond_mock_graph())

        expected_result = {"CRITICAL_EDGES": {"('lib1.so', 'lib9.so')": [["lib1.so", "lib2.so"]]}}
        self.run_analysis(expected_result, libdeps_graph, libdeps.analyzer.CriticalEdges, 'lib1.so',
                          'lib9.so')

        expected_result = {"CRITICAL_EDGES": {"('lib2.so', 'lib9.so')": [["lib5.so", "lib6.so"]]}}
        self.run_analysis(expected_result, libdeps_graph, libdeps.analyzer.CriticalEdges, 'lib2.so',
                          'lib9.so')

        expected_result = {"CRITICAL_EDGES": {"('lib7.so', 'lib8.so')": []}}
        self.run_analysis(expected_result, libdeps_graph, libdeps.analyzer.CriticalEdges, 'lib7.so',
                          'lib8.so')

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
