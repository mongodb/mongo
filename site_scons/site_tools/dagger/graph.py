import sys
import logging
import abc
import json
import copy

import graph_consts

if sys.version_info >= (3, 0):
    basestring = str


class Graph(object):
    """Graph class for storing the build dependency graph. The graph stores the
    directed edges as a nested dict of { RelationshipType: {From_Node: Set of
    connected nodes}} and nodes as a dict of {nodeid : nodeobject}. Can be
    imported from a pickle or JSON file.
    """

    def __init__(self, input=None):
        """
        A graph can be initialized with a .json file, graph object, or with no args
        """
        if isinstance(input, basestring):
            if input.endswith('.json'):
                with open(input, 'r') as f:
                    data = json.load(f, encoding="ascii")
                nodes = {}
                should_fail = False

                for node in data["nodes"]:
                    id = str(node["id"])
                    try:
                        nodes[id] = node_factory(id, int(node["node"]["type"]),
                                                 dict_source=node["node"])
                    except Exception as e:
                        logging.warning("Malformed Data: " + id)
                        should_fail = True

                if should_fail is True:
                    raise ValueError("json nodes are malformed")

                edges = {}

                for edge in data["edges"]:
                    if edge["type"] not in edges:
                        edges[edge["type"]] = {}

                    to_edges = set([e["id"] for e in edge["to_node"]])
                    edges[edge["type"]][edge["from_node"]["id"]] = to_edges

                self._nodes = nodes
                self._edges = edges
        elif isinstance(input, Graph):
            self._nodes = input.nodes
            self._edges = input.edges
        else:
            self._nodes = {}
            self._edges = {}
            for rel in graph_consts.RELATIONSHIP_TYPES:
                self._edges[rel] = {}

    @property
    def nodes(self):
        """We want to ensure that we are not able to mutate
        the nodes or edges properties outside of the specified adder methods
        """
        return copy.deepcopy(self._nodes)

    @property
    def edges(self):
        return copy.deepcopy(self._edges)

    def get_node(self, id):
        return self._nodes.get(id)

    def find_node(self, id, type):
        """returns the node if it exists, otherwise, generates
        it"""
        if self.get_node(id) is not None:
            return self.get_node(id)
        else:
            node = node_factory(id, type)
            self.add_node(node)
            return node

    def get_edge_type(self, edge_type):
        return self._edges[edge_type]

    def add_node(self, node):
        if not isinstance(node, NodeInterface):
            raise TypeError

        if node.id in self._nodes:
            raise ValueError

        self._nodes[node.id] = node

    def add_edge(self, relationship, from_node, to_node):
        if relationship not in graph_consts.RELATIONSHIP_TYPES:
            raise TypeError

        from_node_obj = self.get_node(from_node)
        to_node_obj = self.get_node(to_node)

        if from_node not in self._edges[relationship]:
            self._edges[relationship][from_node] = set()

        if any(item is None for item in (from_node, to_node, from_node_obj, to_node_obj)):
            raise ValueError

        self._edges[relationship][from_node].add(to_node)

        to_node_obj.add_incoming_edges(from_node_obj, self)

    # JSON does not support python sets, so we need to convert each
    # set of edges to lists
    def export_to_json(self, filename="graph.json"):
        node_index = {}

        data = {"edges": [], "nodes": []}

        for idx, id in enumerate(self._nodes.keys()):
            node = self.get_node(id)
            node_index[id] = idx
            node_dict = {}
            node_dict["index"] = idx
            node_dict["id"] = id
            node_dict["node"] = {}

            for property, value in vars(node).iteritems():
                if isinstance(value, set):
                    node_dict["node"][property] = list(value)
                else:
                    node_dict["node"][property] = value

            data["nodes"].append(node_dict)

        for edge_type in graph_consts.RELATIONSHIP_TYPES:
            edges_dict = self._edges[edge_type]
            for node in edges_dict.keys():
                to_nodes = list(self._edges[edge_type][node])
                to_nodes_dicts = [{"index": node_index[to_node], "id": to_node}
                                  for to_node in to_nodes]

                data["edges"].append({"type": edge_type,
                                      "from_node": {"id": node,
                                                    "index": node_index[node]},
                                      "to_node": to_nodes_dicts})

        with open(filename, 'w') as outfile:
            json.dump(data, outfile, indent=4, encoding="ascii")

    def __str__(self):
        return ("<Number of Nodes : {0}, Number of Edges : {1}, "
                "Hash: {2}>").format(len(self._nodes.keys()),
                sum(len(x) for x in self._edges.values()), hash(self))


class NodeInterface(object):
    """Abstract base class for all Node Objects - All nodes must have an id and name
    """
    __metaclass__ = abc.ABCMeta

    @abc.abstractproperty
    def id(self):
        raise NotImplementedError()

    @abc.abstractproperty
    def name(self):
        raise NotImplementedError()


class NodeLib(NodeInterface):
    """NodeLib class which represents a library within the graph
    """
    def __init__(self, id, name, input=None):
        if isinstance(input, dict):
            should_fail = False
            for k, v in input.iteritems():
                try:
                    if isinstance(v, list):
                        setattr(self, k, set(v))
                    else:
                        setattr(self, k, v)
                except AttributeError as e:
                    logging.error("found something bad, {0}, {1}", e, type(e))
                    should_fail = True
            if should_fail:
                raise Exception("Problem setting attribute for NodeLib")
        else:
            self._id = id
            self.type = graph_consts.NODE_LIB
            self._name = name
            self._defined_symbols = set()
            self._defined_files = set()
            self._dependent_files = set()
            self._dependent_libs = set()

    @property
    def id(self):
        return self._id

    @property
    def name(self):
        return self._name

    @property
    def defined_symbols(self):
        return self._defined_symbols

    @defined_symbols.setter
    def defined_symbols(self, value):
        if isinstance(value, set):
            self._defined_symbols = value
        else:
            raise TypeError("NodeLib.defined_symbols must be a set")

    @property
    def defined_files(self):
        return self._defined_files

    @defined_files.setter
    def defined_files(self, value):
        if isinstance(value, set):
            self._defined_files = value
        else:
            raise TypeError("NodeLib.defined_files must be a set")

    @property
    def dependent_files(self):
        return self._dependent_files

    @dependent_files.setter
    def dependent_files(self, value):
        if isinstance(value, set):
            self._dependent_files = value
        else:
            raise TypeError("NodeLib.dependent_files must be a set")

    @property
    def dependent_libs(self):
        return self._dependent_libs

    @dependent_libs.setter
    def dependent_libs(self, value):
        if isinstance(value, set):
            self._defined_libs = value
        else:
            raise TypeError("NodeLib.defined_libs must be a set")

    def add_defined_symbol(self, symbol):
        if symbol is not None:
            self._defined_symbols.add(symbol)

    def add_defined_file(self, file):
        if file is not None:
            self._defined_files.add(file)

    def add_dependent_file(self, file):
        if file is not None:
            self._dependent_files.add(file)

    def add_dependent_lib(self, lib):
        if lib is not None:
            self._dependent_libs.add(lib)

    def add_incoming_edges(self, from_node, g):
        """Whenever you generate a LIB_LIB edge, you must add
        the source lib to the dependent_lib field in the target lib
        """
        if from_node.type == graph_consts.NODE_LIB:
            self.add_dependent_lib(from_node.id)

    def __eq__(self, other):
        if isinstance(other, NodeLib):
            return (self._id == other._id and self._defined_symbols == other._defined_symbols and
                    self._defined_files == other._defined_files and
                    self._dependent_libs == other._dependent_libs and
                    self._dependent_files == other._dependent_files)

        else:
            return False

    def __ne__(self, other):
        return not self.__eq__(other)

    def __str__(self):
        return self.id


class NodeSymbol(NodeInterface):
    """NodeSymbol class which represents a symbol within the dependency graph
    """

    def __init__(self, id, name, input=None):
        if isinstance(input, dict):
            should_fail = False

            for k, v in input.iteritems():
                try:
                    if isinstance(v, list):
                        setattr(self, k, set(v))
                    else:
                        setattr(self, k, v)
                except AttributeError as e:
                    logging.error("found something bad, {0}, {1}", e, type(e))
                    should_fail = True

            if should_fail:
                raise Exception("Problem setting attribute for NodeLib")
        else:
            self._id = id
            self.type = graph_consts.NODE_SYM
            self._name = name
            self._dependent_libs = set()
            self._dependent_files = set()
            self._libs = set()
            self._files = set()

    @property
    def id(self):
        return self._id

    @property
    def name(self):
        return self._name

    @property
    def libs(self):
        return self._libs

    @libs.setter
    def libs(self, value):
        if isinstance(value, set):
            self._libs = value
        else:
            raise TypeError("NodeSymbol.libs must be a set")

    @property
    def files(self):
        return self._files

    @files.setter
    def files(self, value):
        if isinstance(value, set):
            self._files = value
        else:
            raise TypeError("NodeSymbol.files must be a set")

    @property
    def dependent_libs(self):
        return self._dependent_libs

    @dependent_libs.setter
    def dependent_libs(self, value):
        if isinstance(value, set):
            self._dependent_libs = value
        else:
            raise TypeError("NodeSymbol.dependent_libs must be a set")

    @property
    def dependent_files(self):
        return self._dependent_files

    @dependent_files.setter
    def dependent_files(self, value):
        if isinstance(value, set):
            self._dependent_files = value
        else:
            raise TypeError("NodeSymbol.dependent_files must be a set")

    def add_library(self, library):
        if library is not None:
            self._libs.add(library)

    def add_file(self, file):
        if file is not None:
            self._files.add(file)

    def add_dependent_file(self, file):
        if file is not None:
            self._dependent_files.add(file)

    def add_dependent_lib(self, library):
        if library is not None:
            self._dependent_libs.add(library)

    def add_incoming_edges(self, from_node, g):
        if from_node.type == graph_consts.NODE_FILE:
            if from_node.library not in self.libs:
                self.add_dependent_lib(from_node.library)

            self.add_dependent_file(from_node.id)

            lib_node = g.get_node(from_node.library)

            if lib_node is not None and from_node.library not in self.libs:
                g.add_edge(graph_consts.LIB_SYM, lib_node.id, self.id)

    def __eq__(self, other):
        if isinstance(other, NodeSymbol):
            return (self.id == other.id and self._libs == other._libs and
                    self._files == other._files and
                    self._dependent_libs == other._dependent_libs and
                    self._dependent_files == other._dependent_files
                    )
        else:
            return False

    def __ne__(self, other):
        return not self.__eq__(other)

    def __str__(self):
        return self.id


class NodeFile(NodeInterface):
    """NodeFile class which represents an object file within the build dependency graph
    """

    def __init__(self, id, name, input=None):
        if isinstance(input, dict):
            should_fail = False
            for k, v in input.iteritems():
                try:
                    if isinstance(v, list):
                        setattr(self, k, set(v))
                    else:
                        setattr(self, k, v)
                except AttributeError as e:
                    logging.error("found something bad, {0}, {1}", e, type(e))
                    should_fail = True
            if should_fail:
                raise Exception("Problem setting attribute for NodeLib")
        else:
            self._id = id
            self.type = graph_consts.NODE_FILE
            self._name = name
            self._defined_symbols = set()
            self._dependent_libs = set()
            self._dependent_files = set()
            self._lib = None

    @property
    def id(self):
        return self._id

    @property
    def name(self):
        return self._name

    @property
    def defined_symbols(self):
        return self._defined_symbols

    @defined_symbols.setter
    def defined_symbols(self, value):
        if isinstance(value, set):
            self._defined_symbols = value
        else:
            raise TypeError("NodeFile.defined_symbols must be a set")

    @property
    def dependent_libs(self):
        return self._dependent_libs

    @dependent_libs.setter
    def dependent_libs(self, value):
        if isinstance(value, set):
            self._dependent_libs = value
        else:
            raise TypeError("NodeFile.dependent_libs must be a set")

    @property
    def dependent_files(self):
        return self._dependent_files

    @dependent_files.setter
    def dependent_files(self, value):
        if isinstance(value, set):
            self._dependent_files = value
        else:
            raise TypeError("NodeFile.dependent_files must be a set")

    @property
    def library(self):
        return self._lib

    @library.setter
    def library(self, library):
        if library is not None:
            self._lib = library

    def add_defined_symbol(self, symbol):
        if symbol is not None:
            self._defined_symbols.add(symbol)

    def add_dependent_file(self, file):
        if file is not None:
            self._dependent_files.add(file)

    def add_dependent_lib(self, library):
        if library is not None:
            self._dependent_libs.add(library)

    def add_incoming_edges(self, from_node, g):
        if from_node.type == graph_consts.NODE_FILE:
            self.add_dependent_file(from_node.id)
            lib_node = g.get_node(self.library)

            if from_node.library is not None and from_node.library != self.library:
                self.add_dependent_lib(from_node.library)
                g.add_edge(graph_consts.LIB_FIL, from_node.library, self.id)
                if lib_node is not None:
                        lib_node.add_dependent_file(from_node.id)
                        lib_node.add_dependent_lib(from_node.library)
                        g.add_edge(graph_consts.FIL_LIB, from_node.id, lib_node.id)

    def __eq__(self, other):
        if isinstance(other, NodeSymbol):
            return (self.id == other.id and self._lib == other._lib and
                    self._dependent_libs == other._dependent_libs and
                    self._dependent_files == other._dependent_files and
                    self._defined_symbols == other._defined_symbols)

        else:
            return False

    def __ne__(self, other):
        return not self.__eq__(other)

    def __str__(self):
        return self.id


types = {graph_consts.NODE_LIB: NodeLib,
         graph_consts.NODE_SYM: NodeSymbol,
         graph_consts.NODE_FILE: NodeFile}


def node_factory(id, nodetype, dict_source=None):
    if isinstance(dict_source, dict):
        return types[nodetype](id, id, input=dict_source)
    else:
        return types[nodetype](id, id)
