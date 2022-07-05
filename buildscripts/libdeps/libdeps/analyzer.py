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
import textwrap
import copy
import json
import inspect
import functools
from pathlib import Path

import networkx
import cxxfilt

from libdeps.graph import CountTypes, DependsReportTypes, LinterTypes, EdgeProps, NodeProps


class UnsupportedAnalyzer(Exception):
    """Thrown when an analyzer is run on a graph with an unsupported schema."""

    pass


# https://stackoverflow.com/a/25959545/1644736
def get_class_that_defined_method(meth):
    """Get the name of the class for given function."""

    if isinstance(meth, functools.partial):
        return get_class_that_defined_method(meth.func)
    if inspect.ismethod(meth) or (inspect.isbuiltin(meth)
                                  and getattr(meth, '__self__', None) is not None
                                  and getattr(meth.__self__, '__class__', None)):
        for cls in inspect.getmro(meth.__self__.__class__):
            if meth.__name__ in cls.__dict__:
                return cls
        meth = getattr(meth, '__func__', meth)  # fallback to __qualname__ parsing
    if inspect.isfunction(meth):
        cls = getattr(
            inspect.getmodule(meth),
            meth.__qualname__.split('.<locals>', 1)[0].rsplit('.', 1)[0], None)
        if isinstance(cls, type):
            return cls
    return getattr(meth, '__objclass__', None)  # handle special descriptor objects


# newer pylints contain the fix: https://github.com/PyCQA/pylint/pull/2926/commits/35e1c61026eab90af504806ef9da6241b096e659
#signature-mutators=buildscripts.libdeps.graph_analyzer.schema_check
# pylint: disable=no-value-for-parameter
def parametrized(dec):
    """Allow parameters passed to the decorator."""

    def layer(*args, **kwargs):
        def repl(func):
            return dec(func, *args, **kwargs)

        return repl

    return layer


@parametrized
def schema_check(func, schema_version):
    """Check the version for a function against the graph."""

    def check(*args, **kwargs):

        if schema_version <= args[0].graph_schema:
            return func(*args, **kwargs)
        else:
            analyzer = get_class_that_defined_method(func)
            if not analyzer:
                analyzer = "UnknownAnalyzer"
            else:
                analyzer = analyzer.__name__

            raise UnsupportedAnalyzer(
                textwrap.dedent(f"""\


                    ERROR: analysis for '{analyzer}' requires graph schema version '{schema_version}'
                    but detected graph schema version '{args[0].graph_schema}'
                    """))

    return check


class Analyzer:
    """Base class for different types of analyzers."""

    # pylint: disable=too-many-instance-attributes
    def __init__(self, dependency_graph, progress=True):
        """Store the graph and extract the build_dir from the graph."""

        self.graph_schema = dependency_graph.graph.get('graph_schema_version')
        self._dependency_graph = dependency_graph

        self._build_dir = Path(dependency_graph.graph['build_dir'])
        self.deptypes = json.loads(dependency_graph.graph.get('deptypes', "{}"))
        self.set_progress(progress)

    @property
    def _dependents_graph(self):
        if not hasattr(self, 'rgraph'):
            setattr(self, 'rgraph', networkx.reverse_view(self._dependency_graph))
        return self.rgraph

    @_dependents_graph.setter
    def _dependents_graph(self, value):
        self.rgraph = value

    @property
    def _dependency_graph(self):
        if not hasattr(self, 'graph'):
            setattr(self, 'graph', networkx.reverse_view(self._dependents_graph))
        return self.graph

    @_dependency_graph.setter
    def _dependency_graph(self, value):
        self.graph = value

    def get_deptype(self, deptype):
        """Call down to loaded graph to get the deptype from name."""

        return int(self._dependency_graph.get_deptype(deptype))

    def set_progress(self, value=None):
        """Get a progress bar from the loaded graph."""

        self._progressbar = self._dependency_graph.get_progress(value)
        return self._progressbar


class Counter(Analyzer):
    """Base Counter Analyzer class for various counters."""

    def number_of_edge_types(self, edge_type, value):
        """Count the graphs edges based on type."""

        return len([
            edge for edge in self._dependency_graph.edges(data=True)
            if edge[2].get(edge_type) == value
        ])

    def node_type_count(self, node_type, value):
        """Count the graphs nodes based on type."""

        return len([
            node for node in self._dependency_graph.nodes(data=True)
            if node[1].get(node_type) == value
        ])

    def report(self, report):
        """Report the results for the current type."""

        report[self._count_type] = self.run()


class NodeCounter(Counter):
    """Counts and reports number of nodes in the graph."""

    def __init__(self, dependency_graph):
        """Store graph and set type."""

        super().__init__(dependency_graph)
        self._count_type = CountTypes.NODE.name

    @schema_check(schema_version=1)
    def run(self):
        """Count the graphs nodes."""

        return self._dependency_graph.number_of_nodes()


class EdgeCounter(Counter):
    """Counts and reports number of edges in the graph."""

    def __init__(self, dependency_graph):
        """Store graph and set type."""

        super().__init__(dependency_graph)
        self._count_type = CountTypes.EDGE.name

    @schema_check(schema_version=1)
    def run(self):
        """Count the graphs edges."""

        return self._dependency_graph.number_of_edges()


class DirectEdgeCounter(Counter):
    """Counts and reports number of direct edges in the graph."""

    def __init__(self, dependency_graph):
        """Store graph and set type."""

        super().__init__(dependency_graph)
        self._count_type = CountTypes.DIR_EDGE.name

    @schema_check(schema_version=1)
    def run(self):
        """Count the graphs direct edges."""

        return self.number_of_edge_types(EdgeProps.direct.name, True)


class TransEdgeCounter(Counter):
    """Counts and reports number of transitive edges in the graph."""

    def __init__(self, dependency_graph):
        """Store graph and set type."""

        super().__init__(dependency_graph)
        self._count_type = CountTypes.TRANS_EDGE.name

    @schema_check(schema_version=1)
    def run(self):
        """Count the graphs transitive edges."""

        return self.number_of_edge_types(EdgeProps.direct.name, False)


class DirectPubEdgeCounter(Counter):
    """Counts and reports number of direct public edges in the graph."""

    def __init__(self, dependency_graph):
        """Store graph and set type."""

        super().__init__(dependency_graph)
        self._count_type = CountTypes.DIR_PUB_EDGE.name

    @schema_check(schema_version=1)
    def run(self):
        """Count the graphs direct public edges."""
        return len([
            edge for edge in self._dependency_graph.edges(data=True)
            if edge[2].get(EdgeProps.direct.name)
            and edge[2].get(EdgeProps.visibility.name) == int(self.get_deptype('Public'))
        ])


class PublicEdgeCounter(Counter):
    """Counts and reports number of public edges in the graph."""

    def __init__(self, dependency_graph):
        """Store graph and set type."""

        super().__init__(dependency_graph)
        self._count_type = CountTypes.PUB_EDGE.name

    @schema_check(schema_version=1)
    def run(self):
        """Count the graphs public edges."""

        return self.number_of_edge_types(EdgeProps.visibility.name, int(self.get_deptype('Public')))


class PrivateEdgeCounter(Counter):
    """Counts and reports number of private edges in the graph."""

    def __init__(self, dependency_graph):
        """Store graph and set type."""

        super().__init__(dependency_graph)
        self._count_type = CountTypes.PRIV_EDGE.name

    @schema_check(schema_version=1)
    def run(self):
        """Count the graphs private edges."""

        return self.number_of_edge_types(EdgeProps.visibility.name, int(
            self.get_deptype('Private')))


class InterfaceEdgeCounter(Counter):
    """Counts and reports number of interface edges in the graph."""

    def __init__(self, dependency_graph):
        """Store graph and set type."""

        super().__init__(dependency_graph)
        self._count_type = CountTypes.IF_EDGE.name

    @schema_check(schema_version=1)
    def run(self):
        """Count the graphs interface edges."""

        return self.number_of_edge_types(EdgeProps.visibility.name,
                                         int(self.get_deptype('Interface')))


class LibCounter(Counter):
    """Counts and reports number of library nodes in the graph."""

    def __init__(self, dependency_graph):
        """Store graph and set type."""

        super().__init__(dependency_graph)
        self._count_type = CountTypes.LIB.name

    @schema_check(schema_version=1)
    def run(self):
        """Count the graphs lib nodes."""

        return self.node_type_count(NodeProps.bin_type.name, 'SharedLibrary')


class ProgCounter(Counter):
    """Counts and reports number of program nodes in the graph."""

    def __init__(self, dependency_graph):
        """Store graph and set type."""

        super().__init__(dependency_graph)
        self._count_type = CountTypes.PROG.name

    @schema_check(schema_version=1)
    def run(self):
        """Count the graphs program nodes."""

        return self.node_type_count(NodeProps.bin_type.name, 'Program')


def counter_factory(dependency_graph, counters, progressbar=True):
    """Construct counters from a list of strings."""

    counter_map = {
        CountTypes.NODE.name: NodeCounter,
        CountTypes.EDGE.name: EdgeCounter,
        CountTypes.DIR_EDGE.name: DirectEdgeCounter,
        CountTypes.TRANS_EDGE.name: TransEdgeCounter,
        CountTypes.DIR_PUB_EDGE.name: DirectPubEdgeCounter,
        CountTypes.PUB_EDGE.name: PublicEdgeCounter,
        CountTypes.PRIV_EDGE.name: PrivateEdgeCounter,
        CountTypes.IF_EDGE.name: InterfaceEdgeCounter,
        CountTypes.LIB.name: LibCounter,
        CountTypes.PROG.name: ProgCounter,
    }

    if not isinstance(counters, list):
        counters = [counters]

    counter_objs = []
    for counter in counters:
        if counter in counter_map:
            counter_obj = counter_map[counter](dependency_graph)
            counter_obj.set_progress(progressbar)
            counter_objs.append(counter_obj)

        else:
            print(f"Skipping unknown counter: {counter}")

    return counter_objs


class CommonDependents(Analyzer):
    """Finds common dependent nodes for a set of given dependency nodes."""

    def __init__(self, dependency_graph, nodes):
        """Store graph and strip the nodes."""

        super().__init__(dependency_graph)
        self._nodes = nodes

    @schema_check(schema_version=1)
    def run(self):
        """For a given set of nodes, report what nodes depend on all nodes from that set."""

        neighbor_sets = [set(self._dependents_graph[node]) for node in self._nodes]
        return sorted(list(set.intersection(*neighbor_sets)))

    def report(self, report):
        """Add the common depends list for this tuple of nodes."""

        if DependsReportTypes.COMMON_DEPENDS.name not in report:
            report[DependsReportTypes.COMMON_DEPENDS.name] = {}
        report[DependsReportTypes.COMMON_DEPENDS.name][tuple(self._nodes)] = self.run()


class DirectDependents(Analyzer):
    """Finds direct dependent nodes for a given dependency node."""

    def __init__(self, dependency_graph, node):
        """Store graph and strip the node."""

        super().__init__(dependency_graph)
        self._node = node

    @schema_check(schema_version=1)
    def run(self):
        """For given nodes, report what nodes depend directly on that node."""

        return sorted([
            depender for depender in self._dependents_graph[self._node]
            if self._dependents_graph[self._node][depender].get(EdgeProps.direct.name)
        ])

    def report(self, report):
        """Add the direct depends list for this node."""

        if DependsReportTypes.DIRECT_DEPENDS.name not in report:
            report[DependsReportTypes.DIRECT_DEPENDS.name] = {}
        report[DependsReportTypes.DIRECT_DEPENDS.name][self._node] = self.run()


class ExcludeDependents(Analyzer):
    """Finds dependents which depend on the first input node, but exclude the other input nodes."""

    def __init__(self, dependency_graph, nodes):
        """Store graph and strip the nodes."""

        super().__init__(dependency_graph)
        self._nodes = nodes

    @schema_check(schema_version=1)
    def run(self):
        """Find depends with exclusions.

        Given a node, and a set of other nodes, find what nodes depend on the given
        node, but do not depend on the set of nodes.
        """

        valid_depender_nodes = []
        for depender_node in set(self._dependents_graph[self._nodes[0]]):
            if all(
                    bool(excludes_node not in set(self._dependency_graph[depender_node]))
                    for excludes_node in self._nodes[1:]):
                valid_depender_nodes.append(depender_node)
        return sorted(valid_depender_nodes)

    def report(self, report):
        """Add the exclude depends list for this tuple of nodes."""

        if DependsReportTypes.EXCLUDE_DEPENDS.name not in report:
            report[DependsReportTypes.EXCLUDE_DEPENDS.name] = {}
        report[DependsReportTypes.EXCLUDE_DEPENDS.name][tuple(self._nodes)] = self.run()


class InDegreeOne(Analyzer):
    """
    Finds library nodes which have 1 or 0 dependers.

    Such libraries are good candidates for merging or deletion.
    """

    @schema_check(schema_version=1)
    def run(self):
        """Search the graph for in degree 1 or 0 nodes."""

        in_degree_one_nodes = []
        for node, data in self._dependency_graph.nodes(data=True):
            if (len(self._dependents_graph[node]) < 2
                    and data[NodeProps.bin_type.name] == 'SharedLibrary'):

                if len(self._dependents_graph[node]) == 1:
                    depender = list(self._dependents_graph[node].items())[0][0]
                else:
                    depender = None

                in_degree_one_nodes.append([node, depender])

        return sorted(in_degree_one_nodes)

    def report(self, report):
        """Add the indegree one list to the report."""

        report[DependsReportTypes.IN_DEGREE_ONE.name] = self.run()


class GraphPaths(Analyzer):
    """Finds all paths between two nodes in the graph."""

    def __init__(self, dependency_graph, from_node, to_node):
        """Store graph and strip the nodes."""

        super().__init__(dependency_graph)
        self._from_node, self._to_node = from_node, to_node

    @schema_check(schema_version=1)
    def run(self):
        """Find all paths between the two nodes in the graph."""

        # We can really help out networkx path finding algorithm by striping the graph down to
        # just a graph containing only paths between the source and target node. This is done by
        # getting a subtree from the target down, and then getting a subtree of that tree from the
        # source up.
        dependents_tree = self._dependents_graph.get_direct_nonprivate_graph().get_node_tree(
            self._to_node)

        if self._from_node not in dependents_tree:
            return []

        path_tree = networkx.reverse_view(dependents_tree).get_node_tree(self._from_node)
        return list(
            networkx.all_simple_paths(G=path_tree, source=self._from_node, target=self._to_node))

    def report(self, report):
        """Add the path list to the report."""

        if DependsReportTypes.GRAPH_PATHS.name not in report:
            report[DependsReportTypes.GRAPH_PATHS.name] = {}
        report[DependsReportTypes.GRAPH_PATHS.name][tuple([self._from_node,
                                                           self._to_node])] = self.run()


class SymbolDependents(Analyzer):
    """Find all symbol dependents between the two nodes in the graph."""

    def __init__(self, dependency_graph, from_node, to_node):
        """Store graph and strip the nodes."""

        super().__init__(dependency_graph)
        self._from_node, self._to_node = from_node, to_node

    @schema_check(schema_version=1)
    def run(self):
        """Find all symbol dependents between the two nodes in the graph."""

        edge = self._dependents_graph.get_edge_data(u=self._from_node, v=self._to_node)
        if 'symbols' in edge:
            return edge['symbols'].split()
        return []

    def report(self, report):
        """Add the symbol dependents list to the report."""

        if DependsReportTypes.SYMBOL_DEPENDS.name not in report:
            report[DependsReportTypes.SYMBOL_DEPENDS.name] = {}
        report[DependsReportTypes.SYMBOL_DEPENDS.name][tuple([self._from_node,
                                                              self._to_node])] = self.run()


class CriticalEdges(Analyzer):
    """Finds all edges between two nodes, where removing those edges disconnects the two nodes."""

    def __init__(self, dependency_graph, from_node, to_node):
        """Store graph and strip the nodes."""

        super().__init__(dependency_graph)
        self._from_node, self._to_node = from_node, to_node

    @schema_check(schema_version=1)
    def run(self):
        """Use networkx min cut algorithm to find a set of edges."""

        from networkx.algorithms.connectivity import minimum_st_edge_cut

        # The min cut algorithm will get the min cut nearest the end
        # of the direction of the graph, so we we use the reverse graph
        # so that we get a cut nearest our from_node, or the first cut we
        # would encounter on a given path from the from_node to the to_node.
        subgraph = self._dependents_graph.get_direct_nonprivate_graph().get_node_tree(self._to_node)
        if subgraph.has_node(self._from_node):
            min_cut_edges = list(
                minimum_st_edge_cut(G=subgraph, s=self._to_node, t=self._from_node))
        else:
            min_cut_edges = []
        return [(edge[1], edge[0]) for edge in min_cut_edges]

    def report(self, report):
        """Add the critical edges to report."""

        if DependsReportTypes.CRITICAL_EDGES.name not in report:
            report[DependsReportTypes.CRITICAL_EDGES.name] = {}
        report[DependsReportTypes.CRITICAL_EDGES.name][tuple([self._from_node,
                                                              self._to_node])] = self.run()


class UnusedPublicLinter(Analyzer):
    """Lints the graph for any public libdeps that are unused in all resulting transitive edges."""

    def _check_edge_no_symbols(self, edge, original_nodes, checked_edges):
        """Check the edge's transitive tree and made sure no edges have symbols."""

        if edge not in checked_edges:
            checked_edges.add(edge)
            original_node = edge[0]
            depender = edge[1]
            try:
                edge_attribs = self._dependents_graph[original_node][depender]

                if (edge_attribs.get(EdgeProps.visibility.name) == int(self.get_deptype('Public'))
                        or edge_attribs.get(EdgeProps.visibility.name) == int(
                            self.get_deptype('Interface'))):
                    if not edge_attribs.get(EdgeProps.symbols.name):
                        if not self._tree_uses_no_symbols(depender, original_nodes, checked_edges):
                            return False
                    else:
                        return False
            except KeyError:
                pass

        return True

    def _tree_uses_no_symbols(self, node, original_nodes, checked_edges):
        """Recursive walk for a public node.

        Walk the dependency tree for a given Public node, and check if all edges
        in that tree do not have symbol dependencies.
        """

        for depender in self._dependents_graph[node]:
            for original_node in original_nodes:
                edge = (original_node, depender)
                if not self._check_edge_no_symbols(edge, original_nodes, checked_edges):
                    return False
        return True

    def _check_trans_nodes_no_symbols(self, edge, trans_pub_nodes):
        """Check the edge against the transitive nodes for symbols."""

        for trans_node in self._dependency_graph[edge[0]]:
            if (self._dependency_graph[edge[0]][trans_node].get(EdgeProps.visibility.name) == int(
                    self.get_deptype('Public')) or self._dependency_graph[edge[0]][trans_node].get(
                        EdgeProps.visibility.name) == int(self.get_deptype('Interface'))):
                trans_pub_nodes.add(trans_node)
                try:
                    if self._dependents_graph[trans_node][edge[1]].get(EdgeProps.symbols.name):
                        return True
                except KeyError:
                    pass
        return False

    @schema_check(schema_version=1)
    def run(self):
        """Run the unused public linter.

        Run the linter to check for and PUBLIC libdeps which are
        unnecessary and can be converted to PRIVATE.
        """

        unused_public_libdeps = []
        checked_edges = set()

        for edge in self._dependents_graph.edges:
            edge_attribs = self._dependents_graph[edge[0]][edge[1]]

            if (edge_attribs.get(EdgeProps.direct.name)
                    and edge_attribs.get(EdgeProps.visibility.name) == int(
                        self.get_deptype('Public')) and self._dependents_graph.nodes()[edge[1]].get(
                            NodeProps.bin_type.name) == 'SharedLibrary'):

                # First we will get all the transitive libdeps the dependent node
                # induces, while we are getting those we also check if the depender
                # node has any symbol dependencies to that transitive libdep.
                trans_pub_nodes = set([edge[0]])
                found_symbols = self._check_trans_nodes_no_symbols(edge, trans_pub_nodes)

                # If the depender node has no symbol dependencies on the induced libdeps,
                # then we will walk up the tree for the depender node, checking if any of the
                # induced dependencies have symbols. If there are no simples between all transitive
                # edges from this direct public libdep, its safe to change it to public.
                if not found_symbols and self._tree_uses_no_symbols(edge[1], list(trans_pub_nodes),
                                                                    checked_edges):
                    unused_public_libdeps.append((edge[0], edge[1]))

        return unused_public_libdeps

    def report(self, report):
        """Report the lint issies."""

        report[LinterTypes.PUBLIC_UNUSED.name] = self.run()


def linter_factory(dependency_graph, linters, progressbar=True):
    """Construct linters from a list of strings."""

    linter_map = {
        LinterTypes.PUBLIC_UNUSED.name: UnusedPublicLinter,
    }

    if not isinstance(linters, list):
        linters = [linters]

    linters_objs = []
    for linter in linters:
        if linter in linter_map:
            linters_objs.append(linter_map[linter](dependency_graph, progressbar))
        else:
            print(f"Skipping unknown counter: {linter}")

    return linters_objs


class BuildDataReport(Analyzer):
    """Adds the build and graph meta data to the report."""

    @schema_check(schema_version=1)
    def report(self, report):
        """Add the build data from the graph to the report."""

        report['invocation'] = self._dependency_graph.graph.get('invocation')
        report['git_hash'] = self._dependency_graph.graph.get('git_hash')
        report['graph_schema_version'] = self._dependency_graph.graph.get('graph_schema_version')


class LibdepsGraphAnalysis:
    """Runs the given analysis on the input graph."""

    def __init__(self, analysis):
        """Perform analysis based off input args."""

        self._results = {}
        for analyzer in analysis:
            analyzer.report(self._results)

    def get_results(self):
        """Return the results fo the analysis."""

        return self._results

    def run_linters(self, linters):
        """Run the various dependency reports."""

        if LinterTypes.PUBLIC_UNUSED.name in linters:
            self.results[LinterTypes.PUBLIC_UNUSED.name] = \
                self.libdeps_graph.unused_public_linter()


class GaPrinter:
    """Base class for printers of the graph analysis."""

    def __init__(self, libdeps_graph_analysis):
        """Store the graph analysis for use when printing."""

        self._libdeps_graph_analysis = libdeps_graph_analysis


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

        print(self.get_json())

    def get_json(self):
        """Return the results as a JSON string."""

        results = self._libdeps_graph_analysis.get_results()
        return json.dumps(self.serialize(results))


class GaPrettyPrinter(GaPrinter):
    """Printer for pretty console output."""

    _count_descs = {
        CountTypes.NODE.name: "Nodes in Graph: {}",
        CountTypes.EDGE.name: "Edges in Graph: {}",
        CountTypes.DIR_EDGE.name: "Direct Edges in Graph: {}",
        CountTypes.TRANS_EDGE.name: "Transitive Edges in Graph: {}",
        CountTypes.DIR_PUB_EDGE.name: "Direct Public Edges in Graph: {}",
        CountTypes.PUB_EDGE.name: "Public Edges in Graph: {}",
        CountTypes.PRIV_EDGE.name: "Private Edges in Graph: {}",
        CountTypes.IF_EDGE.name: "Interface Edges in Graph: {}",
        CountTypes.LIB.name: "Library Nodes in Graph: {}",
        CountTypes.PROG.name: "Program Nodes in Graph: {}",
    }

    @staticmethod
    def _print_results_node_list(heading, nodes):
        """Util function for printing a list of nodes for depend reports."""

        print(heading)
        for i, depender in enumerate(nodes, start=1):
            print(f"    {i}: {depender}")
        print("")

    def _print_depends_reports(self, results):
        """Print the depends reports result data."""

        # pylint: disable=too-many-branches
        if DependsReportTypes.DIRECT_DEPENDS.name in results:
            print("\nNodes that directly depend on:")
            for node in results[DependsReportTypes.DIRECT_DEPENDS.name]:
                self._print_results_node_list(f'=>depends on {node}:',
                                              results[DependsReportTypes.DIRECT_DEPENDS.name][node])

        if DependsReportTypes.COMMON_DEPENDS.name in results:
            print("\nNodes that commonly depend on:")
            for nodes in results[DependsReportTypes.COMMON_DEPENDS.name]:
                self._print_results_node_list(
                    f'=>depends on {nodes}:',
                    results[DependsReportTypes.COMMON_DEPENDS.name][nodes])

        if DependsReportTypes.EXCLUDE_DEPENDS.name in results:
            print("\nNodes that depend on a node, but exclude others:")
            for nodes in results[DependsReportTypes.EXCLUDE_DEPENDS.name]:
                self._print_results_node_list(
                    f"=>depends: {nodes[0]}, exclude: {nodes[1:]}:",
                    results[DependsReportTypes.EXCLUDE_DEPENDS.name][nodes])

        if DependsReportTypes.GRAPH_PATHS.name in results:
            print("\nDependency graph paths:")
            for nodes in results[DependsReportTypes.GRAPH_PATHS.name]:
                self._print_results_node_list(f"=>start node: {nodes[0]}, end node: {nodes[1]}:", [
                    f"{' -> '.join(path)}"
                    for path in results[DependsReportTypes.GRAPH_PATHS.name][nodes]
                ])

        if DependsReportTypes.CRITICAL_EDGES.name in results:
            print("\nCritical Edges:")
            for nodes in results[DependsReportTypes.CRITICAL_EDGES.name]:
                self._print_results_node_list(
                    f"=>critical edges between {nodes[0]} and {nodes[1]}:",
                    results[DependsReportTypes.CRITICAL_EDGES.name][nodes])

        if DependsReportTypes.IN_DEGREE_ONE.name in results:
            print("\nLibrary nodes with 1 or 0 dependers:")
            for count, nodes in enumerate(results[DependsReportTypes.IN_DEGREE_ONE.name], start=1):
                print(f"    {count}: '{nodes[0]}' <- '{nodes[1]}'")

        if DependsReportTypes.SYMBOL_DEPENDS.name in results:
            print("\nSymbol dependents:")
            for nodes in results[DependsReportTypes.SYMBOL_DEPENDS.name]:
                symbols = results[DependsReportTypes.SYMBOL_DEPENDS.name][nodes]
                print(
                    f"{len(symbols)} symbols defined in '{nodes[0]}' which are used in '{nodes[1]}'"
                )
                for symbol in symbols:
                    print(f"\t{cxxfilt.demangle(symbol)}")

    def print(self):
        """Print the result data."""
        results = self._libdeps_graph_analysis.get_results()

        if 'invocation' in results:
            print(
                textwrap.dedent(f"""\

                    Graph built from git hash:
                    {results['git_hash']}

                    Graph Schema version:
                    {results['graph_schema_version']}

                    Build invocation:
                    {results['invocation']}
                    """))

        for count_type in CountTypes.__members__.items():
            if count_type[0] in self._count_descs and count_type[0] in results:
                print(self._count_descs[count_type[0]].format(results[count_type[0]]))

        self._print_depends_reports(results)

        if LinterTypes.PUBLIC_UNUSED.name in results:
            print(
                f"\nLibdepsLinter: PUBLIC libdeps that could be PRIVATE: {len(results[LinterTypes.PUBLIC_UNUSED.name])}"
            )
            for issue in sorted(results[LinterTypes.PUBLIC_UNUSED.name],
                                key=lambda item: item[1] + item[0]):
                print(f"    {issue[1]}: PUBLIC -> {issue[0]} -> PRIVATE")
