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

import time
import threading
import gc

import flask
import networkx
import cxxfilt
from pympler.asizeof import asizeof
from flask_cors import CORS
from lxml import etree
from flask import request

import libdeps.graph
import libdeps.analyzer


class BackendServer:
    """Create small class for storing variables and state of the backend."""

    def __init__(self, graphml_dir, frontend_url, memory_limit):
        """Create and setup the state variables."""
        self.app = flask.Flask(__name__)
        self.app.config['CORS_HEADERS'] = 'Content-Type'
        CORS(self.app, resources={r"/*": {"origins": frontend_url}})

        self.app.add_url_rule("/api/graphs", "return_graph_files", self.return_graph_files)
        self.app.add_url_rule("/api/graphs/<git_hash>/nodes", "return_node_list",
                              self.return_node_list)
        self.app.add_url_rule("/api/graphs/<git_hash>/analysis", "return_analyze_counts",
                              self.return_analyze_counts)
        self.app.add_url_rule("/api/graphs/<git_hash>/d3", "return_d3", self.return_d3,
                              methods=['POST'])
        self.app.add_url_rule("/api/graphs/<git_hash>/nodes/details", "return_node_infos",
                              self.return_node_infos, methods=['POST'])
        self.app.add_url_rule("/api/graphs/<git_hash>/paths", "return_paths_between",
                              self.return_paths_between, methods=['POST'])

        self.loaded_graphs = {}
        self.total_graph_size = 0
        self.graphml_dir = Path(graphml_dir)
        self.frontend_url = frontend_url
        self.loading_locks = {}
        self.memory_limit_bytes = memory_limit * (10**9) * 0.8
        self.unloading = False
        self.unloading_lock = threading.Lock()

        self.graph_file_tuple = namedtuple('GraphFile', ['version', 'git_hash', 'graph_file'])
        self.graph_files = self.get_graphml_files()

    @staticmethod
    def get_dependency_graph(graph):
        """Returns the dependency graph of a given graph."""

        if graph.graph['graph_schema_version'] == 1:
            return networkx.reverse_view(graph)
        else:
            return graph

    @staticmethod
    def get_dependents_graph(graph):
        """Returns the dependents graph of a given graph."""

        if graph.graph['graph_schema_version'] == 1:
            return graph
        else:
            return networkx.reverse_view(graph)

    def get_app(self):
        """Return the app instance."""

        return self.app

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

    def return_node_infos(self, git_hash):
        """Returns details about a set of selected nodes."""

        req_body = request.get_json()
        if "selected_nodes" in req_body.keys():
            selected_nodes = req_body["selected_nodes"]

            if graph := self.load_graph(git_hash):
                dependents_graph = self.get_dependents_graph(graph)
                dependency_graph = self.get_dependency_graph(graph)

                nodeinfo_data = {'nodeInfos': []}

                for node in selected_nodes:

                    nodeinfo_data['nodeInfos'].append({
                        'id':
                            len(nodeinfo_data['nodeInfos']),
                        'node':
                            str(node),
                        'name':
                            Path(node).name,
                        'attribs': [{
                            'name': key, 'value': value
                        } for key, value in dependents_graph.nodes(data=True)[str(node)].items()],
                        'dependers': [{
                            'node': depender, 'symbols': dependents_graph[str(node)]
                                                         [depender].get('symbols')
                        } for depender in dependents_graph[str(node)]],
                        'dependencies': [{
                            'node': dependency, 'symbols': dependents_graph[dependency]
                                                           [str(node)].get('symbols')
                        } for dependency in dependency_graph[str(node)]],
                    })

                return nodeinfo_data, 200
            return {
                'error': 'Git commit hash (' + git_hash + ') does not have a matching graph file.'
            }, 400
        return {'error': 'Request body does not contain "selected_nodes" attribute.'}, 400

    def return_d3(self, git_hash):
        """Convert the current selected rows into a format for D3."""

        req_body = request.get_json()
        if "selected_nodes" in req_body.keys():
            selected_nodes = req_body["selected_nodes"]

            if graph := self.load_graph(git_hash):
                dependents_graph = self.get_dependents_graph(graph)
                dependency_graph = self.get_dependency_graph(graph)

                nodes = {}
                links = {}
                links_trans = {}

                def add_node_to_graph_data(node):
                    nodes[str(node)] = {
                        'id': str(node), 'name': Path(node).name,
                        'type': dependents_graph.nodes()[str(node)].get('bin_type', '')
                    }

                def add_link_to_graph_data(source, target, data):
                    links[str(source) + str(target)] = {
                        'source': str(source), 'target': str(target), 'data': data
                    }

                for node in selected_nodes:
                    add_node_to_graph_data(node)

                    for libdep in dependency_graph[str(node)]:
                        if dependents_graph[libdep][str(node)].get('direct'):
                            add_node_to_graph_data(libdep)
                            add_link_to_graph_data(node, libdep,
                                                   dependents_graph[libdep][str(node)])

                if "transitive_edges" in req_body.keys() and req_body["transitive_edges"] is True:
                    for node in selected_nodes:
                        for libdep in dependency_graph[str(node)]:
                            if str(libdep) in nodes.keys():
                                add_link_to_graph_data(node, libdep,
                                                       dependents_graph[libdep][str(node)])

                if "extra_nodes" in req_body.keys():
                    extra_nodes = req_body["extra_nodes"]
                    for node in extra_nodes:
                        add_node_to_graph_data(node)

                        for libdep in dependency_graph.get_direct_nonprivate_graph()[str(node)]:
                            add_node_to_graph_data(libdep)
                            add_link_to_graph_data(node, libdep,
                                                   dependents_graph[libdep][str(node)])

                node_data = {
                    'graphData': {
                        'nodes': [data for node, data in nodes.items()],
                        'links': [data for link, data in links.items()],
                        'links_trans': [data for link, data in links_trans.items()],
                    }
                }
                return node_data, 200
            return {
                'error': 'Git commit hash (' + git_hash + ') does not have a matching graph file.'
            }, 400
        return {'error': 'Request body does not contain "selected_nodes" attribute.'}, 400

    def return_analyze_counts(self, git_hash):
        """Perform count analysis and send the results back to frontend."""

        with self.app.test_request_context():
            if graph := self.load_graph(git_hash):
                dependency_graph = self.get_dependency_graph(graph)

                analysis = libdeps.analyzer.counter_factory(
                    dependency_graph,
                    [name[0] for name in libdeps.analyzer.CountTypes.__members__.items()])
                ga = libdeps.analyzer.LibdepsGraphAnalysis(analysis)
                results = ga.get_results()

                graph_data = []
                for i, data in enumerate(results):
                    graph_data.append({'id': i, 'type': data, 'value': results[data]})
                return {'results': graph_data}, 200
            return {
                'error': 'Git commit hash (' + git_hash + ') does not have a matching graph file.'
            }, 400

    def return_paths_between(self, git_hash):
        """Gather all the paths in the graph between a fromNode and toNode."""

        message = request.get_json()
        if "fromNode" in message.keys() and "toNode" in message.keys():
            if graph := self.load_graph(git_hash):
                dependency_graph = self.get_dependency_graph(graph)
                analysis = [
                    libdeps.analyzer.GraphPaths(dependency_graph, message['fromNode'],
                                                message['toNode'])
                ]
                ga = libdeps.analyzer.LibdepsGraphAnalysis(analysis=analysis)
                results = ga.get_results()

                paths = results[libdeps.analyzer.DependsReportTypes.GRAPH_PATHS.name][(
                    message['fromNode'], message['toNode'])]
                paths.sort(key=len)
                nodes = set()
                for path in paths:
                    for node in path:
                        nodes.add(node)

                # Need to handle self.send_graph_data(extra_nodes=list(nodes))
                return {
                    'fromNode': message['fromNode'], 'toNode': message['toNode'], 'paths': paths,
                    'extraNodes': list(nodes)
                }, 200
            return {
                'error': 'Git commit hash (' + git_hash + ') does not have a matching graph file.'
            }, 400
        return {'error': 'Body must contain toNode and fromNode'}, 400

    def return_node_list(self, git_hash):
        """Gather all the nodes in the graph for the node list."""

        with self.app.test_request_context():
            node_data = {'nodes': [], 'links': []}
            if graph := self.load_graph(git_hash):
                for node in sorted(graph.nodes()):
                    node_path = Path(node)
                    node_data['nodes'].append(str(node_path))
                return node_data, 200
            return {
                'error': 'Git commit hash (' + git_hash + ') does not have a matching graph file.'
            }, 400

    def perform_unloading(self, git_hash):
        """Perform the unloading of a graph in a separate thread."""
        if self.total_graph_size > self.memory_limit_bytes:
            while self.total_graph_size > self.memory_limit_bytes:
                self.app.logger.info(
                    f"Current graph memory: {self.total_graph_size / (10**9)} GB, Unloading to get to {self.memory_limit_bytes / (10**9)} GB"
                )

                self.unloading_lock.acquire()

                lru_hash = min(
                    [graph_hash for graph_hash in self.loaded_graphs if graph_hash != git_hash],
                    key=lambda x: self.loaded_graphs[x]['atime'])
                if lru_hash:
                    self.app.logger.info(
                        f"Unloading {[lru_hash]}, last used {round(time.time() - self.loaded_graphs[lru_hash]['atime'] , 1)}s ago"
                    )
                    self.total_graph_size -= self.loaded_graphs[lru_hash]['size']
                    del self.loaded_graphs[lru_hash]
                    del self.loading_locks[lru_hash]
                self.unloading_lock.release()
            gc.collect()
            self.app.logger.info(f"Memory limit satisfied: {self.total_graph_size / (10**9)} GB")
        self.unloading = False

    def unload_graphs(self, git_hash):
        """Unload least recently used graph when hitting application memory threshold."""

        if not self.unloading:
            self.unloading = True

            thread = threading.Thread(target=self.perform_unloading, args=(git_hash, ))
            thread.daemon = True
            thread.start()

    def load_graph(self, git_hash):
        """Load the graph into application memory."""

        with self.app.test_request_context():
            self.unload_graphs(git_hash)

            loaded_graph = None

            self.unloading_lock.acquire()
            if git_hash in self.loaded_graphs:
                self.loaded_graphs[git_hash]['atime'] = time.time()
                loaded_graph = self.loaded_graphs[git_hash]['graph']
            if git_hash not in self.loading_locks:
                self.loading_locks[git_hash] = threading.Lock()
            self.unloading_lock.release()

            self.loading_locks[git_hash].acquire()
            if git_hash not in self.loaded_graphs:
                if git_hash in self.graph_files:
                    file_path = self.graph_files[git_hash].graph_file
                    nx_graph = networkx.read_graphml(file_path)
                    if int(self.get_graph_build_data(file_path).version) > 3:
                        for source, target in nx_graph.edges:
                            try:
                                nx_graph[source][target]['symbols'] = list(
                                    nx_graph[source][target].get('symbols').split('\n'))
                            except AttributeError:
                                nx_graph[source][target]['symbols'] = []
                    else:
                        for source, target in nx_graph.edges:
                            try:
                                nx_graph[source][target]['symbols'] = list(
                                    map(cxxfilt.demangle,
                                        nx_graph[source][target].get('symbols').split()))
                            except AttributeError:
                                try:
                                    nx_graph[source][target]['symbols'] = list(
                                        nx_graph[source][target].get('symbols').split())
                                except AttributeError:
                                    nx_graph[source][target]['symbols'] = []
                    loaded_graph = libdeps.graph.LibdepsGraph(nx_graph)

                    self.loaded_graphs[git_hash] = {
                        'graph': loaded_graph,
                        'size': asizeof(loaded_graph),
                        'atime': time.time(),
                    }
                    self.total_graph_size += self.loaded_graphs[git_hash]['size']
            else:
                loaded_graph = self.loaded_graphs[git_hash]['graph']
            self.loading_locks[git_hash].release()

            return loaded_graph
