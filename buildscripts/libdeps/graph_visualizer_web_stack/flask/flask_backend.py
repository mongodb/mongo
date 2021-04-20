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
Flask backend web server.

The backend interacts with the graph_analyzer to perform queries on various libdeps graphs.
"""

from pathlib import Path
from collections import namedtuple, OrderedDict

import flask
import networkx

from flask_socketio import SocketIO, emit
from flask_cors import CORS
from flask_session import Session
from lxml import etree

import libdeps.graph
import libdeps.analyzer


class BackendServer:
    """Create small class for storing variables and state of the backend."""

    # pylint: disable=too-many-instance-attributes
    def __init__(self, graphml_dir, frontend_url):
        """Create and setup the state variables."""
        self.app = flask.Flask(__name__)
        self.socketio = SocketIO(self.app, cors_allowed_origins=frontend_url)
        self.app.config['CORS_HEADERS'] = 'Content-Type'
        CORS(self.app, resources={r"/*": {"origins": frontend_url}})

        self.app.add_url_rule("/graph_files", "return_graph_files", self.return_graph_files)
        self.socketio.on_event('git_hash_selected', self.git_hash_selected)
        self.socketio.on_event('row_selected', self.row_selected)

        self.loaded_graphs = {}
        self.current_selected_rows = {}
        self.graphml_dir = Path(graphml_dir)
        self.frontend_url = frontend_url

        self.graph_file_tuple = namedtuple('GraphFile', ['version', 'git_hash', 'graph_file'])
        self.graph_files = self.get_graphml_files()

        try:
            default_selected_graph = list(self.graph_files.items())[0][1].graph_file
            self.load_graph_from_file(default_selected_graph)
            self._dependents_graph = networkx.reverse_view(self._dependency_graph)
        except (IndexError, AttributeError) as ex:
            print(ex)
            print(
                f"Failed to load read a graph file from {list(self.graph_files.items())} for graphml_dir '{self.graphml_dir}'"
            )
            exit(1)

    def load_graph_from_file(self, file_path):
        """Load a graph file from disk and handle version."""

        graph = libdeps.graph.LibdepsGraph(networkx.read_graphml(file_path))
        if graph.graph['graph_schema_version'] == 1:
            self._dependents_graph = graph
            self._dependency_graph = networkx.reverse_view(self._dependents_graph)
        else:
            self._dependency_graph = graph
            self._dependents_graph = networkx.reverse_view(self._dependency_graph)

    def get_app(self):
        """Return the app and socketio instances."""

        return self.app, self.socketio

    def get_graph_build_data(self, graph_file):
        """Fast method for extracting basic build data from the graph file."""

        version = ''
        git_hash = ''
        # pylint: disable=c-extension-no-member
        for _, element in etree.iterparse(
                str(graph_file), tag="{http://graphml.graphdrawing.org/xmlns}data"):
            if element.get('key') == 'graph_schema_version':
                version = element.text
            if element.get('key') == 'git_hash':
                git_hash = element.text
            element.clear()
            if version and git_hash:
                break
        return self.graph_file_tuple(version, git_hash, graph_file)

    def get_graphml_files(self):
        """Find all graphml files in the target graphml dir."""

        graph_files = OrderedDict()
        for graph_file in self.graphml_dir.glob("**/*.graphml"):
            graph_file_tuple = self.get_graph_build_data(graph_file)
            graph_files[graph_file_tuple.git_hash[:7]] = graph_file_tuple
        return graph_files

    def return_graph_files(self):
        """Prepare the list of graph files for the frontend."""

        data = {'graph_files': []}
        for i, (_, graph_file_data) in enumerate(self.graph_files.items(), start=1):
            data['graph_files'].append({
                'id': i, 'version': graph_file_data.version, 'git': graph_file_data.git_hash[:7],
                'selected': False
            })
        return data

    def send_node_infos(self):
        """Search through the selected rows and find information about the selected rows."""

        with self.app.test_request_context():

            nodeinfo_data = {'nodeInfos': []}

            for node, _ in self.current_selected_rows.items():

                nodeinfo_data['nodeInfos'].append({
                    'id':
                        len(nodeinfo_data['nodeInfos']),
                    'node':
                        str(node),
                    'name':
                        node.name,
                    'attribs': [{
                        'name': key, 'value': value
                    } for key, value in self._dependents_graph.nodes(data=True)[str(node)].items()],
                    'dependers': [{
                        'node':
                            depender, 'symbols':
                                self._dependents_graph[str(node)][depender].get('symbols',
                                                                                '').split(' ')
                    } for depender in self._dependents_graph[str(node)]],
                    'dependencies': [{
                        'node':
                            dependency, 'symbols':
                                self._dependents_graph[dependency][str(node)].get('symbols',
                                                                                  '').split(' ')
                    } for dependency in self._dependency_graph[str(node)]],
                })

            self.socketio.emit("node_infos", nodeinfo_data)

    def send_graph_data(self):
        """Convert the current selected rows into a format for D3."""

        with self.app.test_request_context():

            nodes = set()
            links = set()

            for node, _ in self.current_selected_rows.items():
                nodes.add(
                    tuple({
                        'id': str(node), 'name': node.name, 'type': self._dependents_graph.nodes()
                                                                    [str(node)]['bin_type']
                    }.items()))

                for depender in self._dependency_graph[str(node)]:

                    depender_path = Path(depender)
                    if self._dependents_graph[depender][str(node)].get('direct'):
                        nodes.add(
                            tuple({
                                'id':
                                    str(depender_path), 'name':
                                        depender_path.name, 'type':
                                            self._dependents_graph.nodes()[str(depender_path)]
                                            ['bin_type']
                            }.items()))
                        links.add(
                            tuple({'source': str(node), 'target': str(depender_path)}.items()))

            node_data = {
                'graphData': {
                    'nodes': [dict(node) for node in nodes],
                    'links': [dict(link) for link in links],
                }, 'selectedNodes': [str(node) for node in list(self.current_selected_rows.keys())]
            }
            self.socketio.emit("graph_data", node_data)

    def row_selected(self, message):
        """Construct the new graphData nodeInfo when a cell is selected."""

        print(f"Got row {message}!")

        if message['isSelected'] == 'flip':
            if message['data']['node'] in self.current_selected_rows:
                self.current_selected_rows.pop(message['data']['node'])
            else:
                self.current_selected_rows[Path(message['data']['node'])] = message['data']
        else:
            if message['isSelected'] and message:
                self.current_selected_rows[Path(message['data']['node'])] = message['data']
            else:
                self.current_selected_rows.pop(message['data']['node'])

        self.socketio.start_background_task(self.send_graph_data)
        self.socketio.start_background_task(self.send_node_infos)

    def analyze_counts(self):
        """Perform count analysis and send the results back to frontend."""

        with self.app.test_request_context():

            analysis = libdeps.analyzer.counter_factory(
                self._dependents_graph,
                [name[0] for name in libdeps.analyzer.CountTypes.__members__.items()])
            ga = libdeps.analyzer.LibdepsGraphAnalysis(analysis)
            results = ga.get_results()

            graph_data = []
            for i, data in enumerate(results):
                graph_data.append({'id': i, 'type': data, 'value': results[data]})
            self.socketio.emit("graph_results", graph_data)

    def send_node_list(self):
        """Gather all the nodes in the graph for the node list."""

        with self.app.test_request_context():
            node_data = {
                'graphData': {'nodes': [], 'links': []},
                "selectedNodes": [str(node) for node in list(self.current_selected_rows.keys())]
            }

            for node in self._dependents_graph.nodes():
                node_path = Path(node)
                node_data['graphData']['nodes'].append(
                    {'id': str(node_path), 'name': node_path.name})
            self.socketio.emit("graph_nodes", node_data)

    def load_graph(self, message):
        """Load the graph into application memory and kick off threads for analysis on new graph."""

        with self.app.test_request_context():

            current_hash = self._dependents_graph.graph.get('git_hash', 'NO_HASH')[:7]
            if current_hash != message['hash']:
                self.current_selected_rows = {}
                if message['hash'] in self.loaded_graphs:
                    self._dependents_graph = self.loaded_graphs[message['hash']]
                    self._dependents_graph = networkx.reverse_view(self._dependency_graph)
                else:
                    print(
                        f'loading new graph {current_hash} because different than {message["hash"]}'
                    )

                    self.load_graph_from_file(self.graph_files[message['hash']].graph_file)
                    self.loaded_graphs[message['hash']] = self._dependents_graph

            self.socketio.start_background_task(self.analyze_counts)
            self.socketio.start_background_task(self.send_node_list)
            self.socketio.emit("graph_data", {'graphData': {'nodes': [], 'links': []}})

    def git_hash_selected(self, message):
        """Load the new graph and perform queries on it."""

        print(f"Got requests2 {message}!")

        emit("other_hash_selected", message, broadcast=True)

        self.socketio.start_background_task(self.load_graph, message)
