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
Graph Analysis Command Line Interface.

A Command line interface to the graph analysis module.
"""

import argparse
import textwrap
import sys
from pathlib import Path
import copy

import networkx

import libdeps.analyzer as libdeps_analyzer
from libdeps.graph import LibdepsGraph, CountTypes, LinterTypes


class LinterSplitArgs(argparse.Action):
    """Custom argument action for checking multiple choice comma separated list."""

    def __call__(self, parser, namespace, values, option_string=None):
        """Create a multi choice comma separated list."""

        selected_choices = [v.upper() for v in ''.join(values).split(',') if v]
        invalid_choices = [
            choice for choice in selected_choices if choice not in self.valid_choices
        ]
        if invalid_choices:
            raise Exception(
                f"Invalid choices: {invalid_choices}\nMust use choices from {self.valid_choices}")
        if CountTypes.ALL.name in selected_choices:
            selected_choices = copy.copy(self.valid_choices)
            selected_choices.remove(CountTypes.ALL.name)
        if selected_choices == []:
            selected_choices = copy.copy(self.default_choices)
        setattr(namespace, self.dest, [opt.replace('-', '_') for opt in selected_choices])


class CountSplitArgs(LinterSplitArgs):
    """Special case of common custom arg action for Count types."""

    valid_choices = [name[0].replace('_', '-') for name in CountTypes.__members__.items()]
    default_choices = [
        name[0] for name in CountTypes.__members__.items() if name[0] != CountTypes.ALL.name
    ]


class LintSplitArgs(LinterSplitArgs):
    """Special case of common custom arg action for Count types."""

    valid_choices = [name[0].replace('_', '-') for name in LinterTypes.__members__.items()]
    default_choices = [LinterTypes.PUBLIC_UNUSED.name]


class CustomFormatter(argparse.RawTextHelpFormatter, argparse.ArgumentDefaultsHelpFormatter):
    """Custom arg help formatter for modifying the defaults printed for the custom list action."""

    @staticmethod
    def _get_help_length(enum_type):
        max_length = max([len(name[0]) for name in enum_type.__members__.items()])
        help_text = {}
        for name in enum_type.__members__.items():
            help_text[name[0]] = name[0].lower() + ('-' * (max_length - len(name[0]))) + ": "
        return help_text

    def _get_help_string(self, action):

        if isinstance(action, CountSplitArgs):
            help_text = self._get_help_length(CountTypes)
            return textwrap.dedent(f"""\
                {action.help}
                default: all, choices:
                    {help_text[CountTypes.ALL.name]}perform all counts
                    {help_text[CountTypes.NODE.name]}count nodes
                    {help_text[CountTypes.EDGE.name]}count edges
                    {help_text[CountTypes.DIR_EDGE.name]}count edges declared directly on a node
                    {help_text[CountTypes.TRANS_EDGE.name]}count edges induced by direct public edges
                    {help_text[CountTypes.DIR_PUB_EDGE.name]}count edges that are directly public
                    {help_text[CountTypes.PUB_EDGE.name]}count edges that are public
                    {help_text[CountTypes.PRIV_EDGE.name]}count edges that are private
                    {help_text[CountTypes.IF_EDGE.name]}count edges that are interface
                    {help_text[CountTypes.LIB.name]}count library nodes
                    {help_text[CountTypes.PROG.name]}count program nodes
                """)
        elif isinstance(action, LintSplitArgs):
            help_text = self._get_help_length(LinterTypes)
            return textwrap.dedent(f"""\
                {action.help}
                default: all, choices:
                    {help_text[LinterTypes.ALL.name]}perform all linters
                    {help_text[LinterTypes.PUBLIC_UNUSED.name]}find unnecessary public libdeps
                """)
        return super()._get_help_string(action)


def setup_args_parser():
    """Add and parse the input args."""

    parser = argparse.ArgumentParser(formatter_class=CustomFormatter)

    parser.add_argument('--graph-file', type=str, action='store', help="The LIBDEPS graph to load.",
                        default="build/opt/libdeps/libdeps.graphml")

    parser.add_argument('--format', choices=['pretty', 'json'], default='pretty',
                        help="The output format type.")

    parser.add_argument('--build-data', choices=['on', 'off'], default='on',
                        help="Print the invocation and git hash used to build the graph")

    parser.add_argument('--counts', metavar='COUNT,', nargs='*', action=CountSplitArgs,
                        default=CountSplitArgs.default_choices,
                        help="Output various counts from the graph. Comma separated list.")

    parser.add_argument('--lint', metavar='LINTER,', nargs='*', action=LintSplitArgs,
                        default=LintSplitArgs.default_choices,
                        help="Perform various linters on the graph. Comma separated list.")

    parser.add_argument('--direct-depends', action='append', default=[],
                        help="Print the nodes which depends on a given node.")

    parser.add_argument('--common-depends', nargs='+', action='append', default=[],
                        help="Print the nodes which have a common dependency on all N nodes.")

    parser.add_argument(
        '--exclude-depends', nargs='+', action='append', default=[], help=
        "Print nodes which depend on the first node of N nodes, but exclude all nodes listed there after."
    )

    parser.add_argument('--graph-paths', nargs='+', action='append', default=[],
                        help="[from_node] [to_node]: Print all paths between 2 nodes.")

    parser.add_argument(
        '--critical-edges', nargs='+', action='append', default=[], help=
        "[from_node] [to_node]: Print edges between two nodes, which if removed would break the dependency between those "
        + "nodes,.")

    parser.add_argument(
        '--symbol-depends', nargs='+', action='append', default=[],
        help="[from_node] [to_node]: Print symbols defined in from_node used by to_node.")

    parser.add_argument(
        '--indegree-one', action='store_true', default=False, help=
        "Find candidate nodes for merging by searching the graph for nodes with only one node which depends on them."
    )

    args = parser.parse_args()

    for arg_list in args.graph_paths:
        if len(arg_list) != 2:
            parser.error(
                f'Must pass two args for --graph-paths, [from_node] [to_node], not {arg_list}')

    for arg_list in args.critical_edges:
        if len(arg_list) != 2:
            parser.error(
                f'Must pass two args for --critical-edges, [from_node] [to_node], not {arg_list}')

    for arg_list in args.symbol_depends:
        if len(arg_list) != 2:
            parser.error(
                f'Must pass two args for --symbol-depends, [from_node] [to_node], not {arg_list}')

    return parser.parse_args()


def strip_build_dir(build_dir, node):
    """Small util function for making args match the graph paths."""

    return str(Path(node).relative_to(build_dir))


def strip_build_dirs(build_dir, nodes):
    """Small util function for making a list of nodes match graph paths."""

    return [strip_build_dir(build_dir, node) for node in nodes]


def load_graph_data(graph_file, output_format):
    """Load a graphml file."""

    if output_format == "pretty":
        sys.stdout.write("Loading graph data...")
        sys.stdout.flush()
    graph = networkx.read_graphml(graph_file)
    if output_format == "pretty":
        sys.stdout.write("Loaded!\n\n")
    return graph


def main():
    """Perform graph analysis based on input args."""

    args = setup_args_parser()
    graph = load_graph_data(args.graph_file, args.format)
    libdeps_graph = LibdepsGraph(graph=graph)
    build_dir = libdeps_graph.graph['build_dir']

    if libdeps_graph.graph['graph_schema_version'] == 1:
        libdeps_graph = networkx.reverse_view(libdeps_graph)

    analysis = libdeps_analyzer.counter_factory(libdeps_graph, args.counts)

    for analyzer_args in args.direct_depends:
        analysis.append(
            libdeps_analyzer.DirectDependents(libdeps_graph,
                                              strip_build_dir(build_dir, analyzer_args)))

    for analyzer_args in args.common_depends:
        analysis.append(
            libdeps_analyzer.CommonDependents(libdeps_graph,
                                              strip_build_dirs(build_dir, analyzer_args)))

    for analyzer_args in args.exclude_depends:
        analysis.append(
            libdeps_analyzer.ExcludeDependents(libdeps_graph,
                                               strip_build_dirs(build_dir, analyzer_args)))

    for analyzer_args in args.graph_paths:
        analysis.append(
            libdeps_analyzer.GraphPaths(libdeps_graph, strip_build_dir(build_dir, analyzer_args[0]),
                                        strip_build_dir(build_dir, analyzer_args[1])))

    for analyzer_args in args.symbol_depends:
        analysis.append(
            libdeps_analyzer.SymbolDependents(libdeps_graph,
                                              strip_build_dir(build_dir, analyzer_args[0]),
                                              strip_build_dir(build_dir, analyzer_args[1])))

    for analyzer_args in args.critical_edges:
        analysis.append(
            libdeps_analyzer.CriticalEdges(libdeps_graph,
                                           strip_build_dir(build_dir, analyzer_args[0]),
                                           strip_build_dir(build_dir, analyzer_args[1])))

    if args.indegree_one:
        analysis.append(libdeps_analyzer.InDegreeOne(libdeps_graph))

    analysis += libdeps_analyzer.linter_factory(libdeps_graph, args.lint)

    if args.build_data:
        analysis.append(libdeps_analyzer.BuildDataReport(libdeps_graph))

    ga = libdeps_analyzer.LibdepsGraphAnalysis(analysis)

    if args.format == 'pretty':
        ga_printer = libdeps_analyzer.GaPrettyPrinter(ga)
    elif args.format == 'json':
        ga_printer = libdeps_analyzer.GaJsonPrinter(ga)
    else:
        return

    ga_printer.print()


if __name__ == "__main__":
    main()
