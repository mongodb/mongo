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

import networkx
import graph_analyzer
from libdeps_graph_enums import CountTypes, LinterTypes

class LinterSplitArgs(argparse.Action):
    """Custom argument action for checking multiple choice comma separated list."""

    def __call__(self, parser, namespace, values, option_string=None):
        """Create a multi choice comma separated list."""

        selected_choices = [v for v in ''.join(values).split(',') if v]
        invalid_choices = [
            choice for choice in selected_choices if choice not in self.valid_choices
        ]
        if invalid_choices:
            raise Exception(
                f"Invalid choices: {invalid_choices}\nMust use choices from {self.valid_choices}")
        if 'all' in selected_choices or selected_choices == []:
            selected_choices = self.valid_choices
            selected_choices.remove('all')
        setattr(namespace, self.dest, [opt.replace('-', '_') for opt in selected_choices])


class CountSplitArgs(LinterSplitArgs):
    """Special case of common custom arg action for Count types."""

    valid_choices = [name[0].replace('_', '-') for name in CountTypes.__members__.items()]


class LintSplitArgs(LinterSplitArgs):
    """Special case of common custom arg action for Count types."""

    valid_choices = [name[0].replace('_', '-') for name in LinterTypes.__members__.items()]


class CustomFormatter(argparse.RawTextHelpFormatter, argparse.ArgumentDefaultsHelpFormatter):
    """Custom arg help formatter for modifying the defaults printed for the custom list action."""

    @staticmethod
    def _get_help_length(enum_type):
        max_length = max([len(name[0]) for name in enum_type.__members__.items()])
        count_help = {}
        for name in enum_type.__members__.items():
            count_help[name[0]] = name[0] + ('-' * (max_length - len(name[0]))) + ": "
        return count_help

    def _get_help_string(self, action):

        if isinstance(action, CountSplitArgs):
            count_help = self._get_help_length(CountTypes)
            return textwrap.dedent(f"""\
                {action.help}
                default: all, choices:
                    {count_help[CountTypes.all.name]}perform all counts
                    {count_help[CountTypes.node.name]}count nodes
                    {count_help[CountTypes.edge.name]}count edges
                    {count_help[CountTypes.dir_edge.name]}count edges declared directly on a node
                    {count_help[CountTypes.trans_edge.name]}count edges induced by direct public edges
                    {count_help[CountTypes.dir_pub_edge.name]}count edges that are directly public
                    {count_help[CountTypes.pub_edge.name]}count edges that are public
                    {count_help[CountTypes.priv_edge.name]}count edges that are private
                    {count_help[CountTypes.if_edge.name]}count edges that are interface
                    {count_help[CountTypes.shim.name]}count shim nodes
                    {count_help[CountTypes.lib.name]}count library nodes
                    {count_help[CountTypes.prog.name]}count program nodes
                """)
        elif isinstance(action, LintSplitArgs):
            count_help = self._get_help_length(LinterTypes)
            return textwrap.dedent(f"""\
                {action.help}
                default: all, choices:
                    {count_help[LinterTypes.all.name]}perform all linters
                    {count_help[LinterTypes.node.name]}find unnecessary public libdeps
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

    parser.add_argument(
        '--counts', metavar='COUNT,', nargs='*', action=CountSplitArgs,
        default=[name[0] for name in CountTypes.__members__.items() if name[0] != 'all'],
        help="Output various counts from the graph. Comma separated list.")

    parser.add_argument(
        '--lint', metavar='LINTER,', nargs='*', action=LintSplitArgs,
        default=[name[0] for name in LinterTypes.__members__.items() if name[0] != 'all'],
        help="Perform various linters on the graph. Comma separated list.")

    parser.add_argument('--direct-depends', action='append', default=[],
                        help="Print the nodes which depends on a given node.")

    parser.add_argument('--common-depends', nargs='+', action='append', default=[],
                        help="Print the nodes which have a common dependency on all N nodes.")

    parser.add_argument(
        '--exclude-depends', nargs='+', action='append', default=[], help=
        "Print nodes which depend on the first node of N nodes, but exclude all nodes listed there after."
    )

    return parser.parse_args()


def load_graph_data(graph_file, output_format):
    """Load a graphml file into a LibdepsGraph."""

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
    libdeps = graph_analyzer.LibdepsGraph(graph)

    analysis = graph_analyzer.counter_factory(libdeps, args.counts)

    for depends in args.direct_depends:
        analysis.append(graph_analyzer.DirectDependencies(libdeps, depends))

    for depends in args.common_depends:
        analysis.append(graph_analyzer.CommonDependencies(libdeps, depends))

    for depends in args.exclude_depends:
        analysis.append(graph_analyzer.ExcludeDependencies(libdeps, depends))

    analysis += graph_analyzer.linter_factory(libdeps, args.lint)

    if args.build_data:
        analysis.append(graph_analyzer.BuildDataReport(libdeps))

    ga = graph_analyzer.LibdepsGraphAnalysis(libdeps_graph=libdeps, analysis=analysis)

    if args.format == 'pretty':
        ga_printer = graph_analyzer.GaPrettyPrinter(ga)
    elif args.format == 'json':
        ga_printer = graph_analyzer.GaJsonPrinter(ga)
    else:
        return

    ga_printer.print()


if __name__ == "__main__":
    main()
