#!/usr/bin/env python
#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.

import argparse
import ast
import os

import networkx as nx

from parse_wt_ast import parse_wiredtiger_files
from build_dependency_graph import build_graph
from query_dependency_graph import who_uses, who_is_used_by, explain_cycle, privacy_report, generate_dependency_file

def parse_args():
    parser = argparse.ArgumentParser(description="TODO")

    subparsers = parser.add_subparsers(dest='command', required=True)

    who_uses_parser = subparsers.add_parser('who_uses', help='Who uses this module?')
    who_uses_parser.add_argument('module', type=str, help='module name')

    who_is_used_by_parser = subparsers.add_parser(
        'who_is_used_by', help='Who this module is used by')
    who_is_used_by_parser.add_argument('module', type=str, help='module name')

    list_cycles_parser = subparsers.add_parser('list_cycles', help='List cycles present in the dependency graph')
    list_cycles_parser.add_argument('module', type=str, help='module name')

    explain_cycle_parser = subparsers.add_parser('explain_cycle', help='Describe the links that create a given cycle')
    explain_cycle_parser.add_argument('cycle', type=str, 
        help="The cycle to explain. It must be in the format \"['meta', 'conn', 'log']\"")

    privacy_check_parser = subparsers.add_parser('privacy_report', 
        help='Report which structs and struct fields in the module are private to the module')
    privacy_check_parser.add_argument('module', type=str, help='module name')

    subparsers.add_parser('generate_dependency_file', 
        help='Generate a file that reports all dependencies')

    return parser.parse_args()

def main():

    args = parse_args()

    parsed_files = parse_wiredtiger_files(debug=False)
    graph, ambiguous_fields = build_graph(parsed_files)

    if args.command == "who_uses":
        who_uses(args.module, graph)
    elif args.command == "who_is_used_by":
        who_is_used_by(args.module, graph)
    elif args.command == "list_cycles":
        # Report the smallest cycles last so they're more visible
        for c in sorted(nx.simple_cycles(graph, length_bound=3), key=lambda x: -len(x)):
            if args.module in c:
                print(c)
    elif args.command == "explain_cycle":
        cycle = ast.literal_eval(args.cycle)
        explain_cycle(cycle, graph)
    elif args.command == "privacy_report":
        privacy_report(args.module, graph, parsed_files, ambiguous_fields)
    elif args.command == "generate_dependency_file":
        generate_dependency_file(graph)
    else:
        print(f"Unrecognized command {args.command}!")
        exit(1)

if __name__ == "__main__":
    # Always run this script from its containing directory. We have some 
    # hardcoded paths that require it.
    working_dir = os.path.dirname(os.path.abspath(__file__))
    os.chdir(working_dir)
    main()
