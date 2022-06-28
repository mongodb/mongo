#!/usr/bin/env python3
#
# Copyright 2022 MongoDB Inc.
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
"""Test graphs for the graph visualizer and analyzer."""

import json
import argparse
import networkx

from libdeps.graph import LibdepsGraph, EdgeProps, NodeProps


def get_args():
    """Create the argparse and return passed args."""

    parser = argparse.ArgumentParser()

    parser.add_argument('--graph-output-dir', type=str, action='store',
                        help="Directory test graphml files will be saved.",
                        default="build/opt/libdeps")
    return parser.parse_args()


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
    graph.graph['deptypes'] = json.dumps({
        "Global": 0,
        "Public": 1,
        "Private": 2,
        "Interface": 3,
    })
    graph.graph['git_hash'] = 'TEST001'

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
    graph.graph['deptypes'] = json.dumps({
        "Global": 0,
        "Public": 1,
        "Private": 2,
        "Interface": 3,
    })
    graph.graph['git_hash'] = 'TEST002'

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


def get_basic_mock_directory_graph():
    """Construct a mock graph which covers most cases and is easy to understand."""

    graph = LibdepsGraph()
    graph.graph['build_dir'] = '.'
    graph.graph['graph_schema_version'] = 2
    graph.graph['deptypes'] = json.dumps({
        "Global": 0,
        "Public": 1,
        "Private": 2,
        "Interface": 3,
    })
    graph.graph['git_hash'] = 'TEST003'

    # builds a graph of mostly public edges:
    #
    #                    /-lib5.so
    #               /lib3
    #              |     \-lib6.so
    # <-lib1.so--lib2
    #              |       /-lib5.so (private)
    #               \lib4.so
    #                      \-lib6.so

    # nodes
    add_node(graph, 'dir1/lib1.so', 'SharedLibrary')
    add_node(graph, 'dir1/sub1/lib2', 'Program')
    add_node(graph, 'dir1/sub1/lib3', 'Program')
    add_node(graph, 'dir1/sub2/lib4.so', 'SharedLibrary')
    add_node(graph, 'dir2/lib5.so', 'SharedLibrary')
    add_node(graph, 'dir2/lib6.so', 'SharedLibrary')

    # direct edges
    add_edge(graph, 'dir1/lib1.so', 'dir1/sub1/lib2', direct=True,
             visibility=graph.get_deptype('Public'))
    add_edge(graph, 'dir1/sub1/lib2', 'dir1/sub1/lib3', direct=True,
             visibility=graph.get_deptype('Public'))
    add_edge(graph, 'dir1/sub1/lib2', 'dir1/sub2/lib4.so', direct=True,
             visibility=graph.get_deptype('Public'))
    add_edge(graph, 'dir1/sub2/lib4.so', 'dir2/lib6.so', direct=True,
             visibility=graph.get_deptype('Public'))
    add_edge(graph, 'dir1/sub1/lib3', 'dir2/lib5.so', direct=True,
             visibility=graph.get_deptype('Public'))
    add_edge(graph, 'dir1/sub1/lib3', 'dir2/lib6.so', direct=True,
             visibility=graph.get_deptype('Public'))
    add_edge(graph, 'dir1/sub2/lib4.so', 'dir2/lib5.so', direct=True,
             visibility=graph.get_deptype('Private'))

    # trans for 3
    add_edge(graph, 'dir1/lib1.so', 'dir1/sub1/lib3', direct=False,
             visibility=graph.get_deptype('Public'))

    # trans for 4
    add_edge(graph, 'dir1/lib1.so', 'dir1/sub2/lib4.so', direct=False,
             visibility=graph.get_deptype('Public'))

    # trans for 5
    add_edge(graph, 'dir1/sub1/lib2', 'dir2/lib5.so', direct=False,
             visibility=graph.get_deptype('Public'))
    add_edge(graph, 'dir1/lib1.so', 'dir2/lib5.so', direct=False,
             visibility=graph.get_deptype('Public'))

    # trans for 6
    add_edge(graph, 'dir1/sub1/lib2', 'dir2/lib6.so', direct=False,
             visibility=graph.get_deptype('Public'))
    add_edge(graph, 'dir1/lib1.so', 'dir2/lib6.so', direct=False,
             visibility=graph.get_deptype('Public'))

    return graph


def get_simple_directory_graph():
    """Construct a mock graph which covers most cases and is easy to understand."""

    graph = LibdepsGraph()
    graph.graph['build_dir'] = '.'
    graph.graph['graph_schema_version'] = 2
    graph.graph['deptypes'] = json.dumps({
        "Global": 0,
        "Public": 1,
        "Private": 2,
        "Interface": 3,
    })
    graph.graph['git_hash'] = 'TEST004'

    #        lib2.so <- lib4.so
    #       /∧     \∨
    # lib1.so     prog1 <- lib5.so
    #       \∨     /∧
    #        lib3.so -> prog2

    # nodes
    add_node(graph, 'mongo/base/lib1.so', 'SharedLibrary')
    add_node(graph, 'mongo/base/lib2.so', 'SharedLibrary')
    add_node(graph, 'mongo/db/lib3.so', 'SharedLibrary')
    add_node(graph, 'third_party/lib4.so', 'SharedLibrary')
    add_node(graph, 'third_party/lib5.so', 'SharedLibrary')
    add_node(graph, 'mongo/base/prog1', 'Program')
    add_node(graph, 'mongo/db/prog2', 'Program')

    # direct edges
    add_edge(graph, 'mongo/base/lib1.so', 'mongo/base/lib2.so', direct=True,
             visibility=graph.get_deptype('Public'))
    add_edge(graph, 'mongo/base/lib1.so', 'mongo/db/lib3.so', direct=True,
             visibility=graph.get_deptype('Public'))
    add_edge(graph, 'mongo/base/lib2.so', 'mongo/base/prog1', direct=True,
             visibility=graph.get_deptype('Public'))
    add_edge(graph, 'mongo/db/lib3.so', 'mongo/base/prog1', direct=True,
             visibility=graph.get_deptype('Public'))
    add_edge(graph, 'mongo/db/lib3.so', 'mongo/db/prog2', direct=True,
             visibility=graph.get_deptype('Public'))
    add_edge(graph, 'third_party/lib4.so', 'mongo/base/lib2.so', direct=True,
             visibility=graph.get_deptype('Public'))
    add_edge(graph, 'third_party/lib5.so', 'mongo/base/prog1', direct=True,
             visibility=graph.get_deptype('Public'))

    return graph


def save_graph_file(graph, output_dir):
    """Save a graph locally as a .graphml."""

    filename = output_dir + "/libdeps_" + graph.graph['git_hash'] + ".graphml"
    networkx.write_graphml(graph, filename, named_key_ids=True)


def main():
    """Generate and save the test graphs as .graphml files."""

    args = get_args()
    output_dir = args.graph_output_dir

    graph = get_double_diamond_mock_graph()
    save_graph_file(graph, output_dir)

    graph = get_basic_mock_graph()
    save_graph_file(graph, output_dir)

    graph = get_basic_mock_directory_graph()
    save_graph_file(graph, output_dir)

    graph = get_simple_directory_graph()
    save_graph_file(graph, output_dir)


if __name__ == "__main__":
    main()
