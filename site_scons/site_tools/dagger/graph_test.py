"""Tests for the graph class used in the dagger tool. Tests the add_edge and
add_node methods, along with the methods for exporting and importing the graph
from JSON
"""

import json
import unittest
import graph
import graph_consts


def generate_graph():
    """Generates our test graph"""

    g = graph.Graph()
    sym1 = graph.NodeSymbol("sym1", "sym1")

    lib1 = graph.NodeLib("lib1", "lib1")
    lib2 = graph.NodeLib("lib2", "lib2")
    lib3 = graph.NodeLib("lib3", "lib3")

    file1 = graph.NodeFile("file1", "file1")
    file2 = graph.NodeFile("file2", "file2")
    file3 = graph.NodeFile("file3", "file3")

    lib_sym = graph.NodeLib("lib_sym", "lib_sym")
    file_sym = graph.NodeFile("file_sym", "file_sym")

    g.add_node(sym1)
    g.add_node(lib1)
    g.add_node(lib2)
    g.add_node(lib3)
    g.add_node(file1)
    g.add_node(file2)
    g.add_node(file3)
    g.add_node(lib_sym)
    g.add_node(file_sym)

    sym1.add_file(file_sym.id)
    sym1.add_library(lib_sym.id)
    lib_sym.add_defined_symbol(sym1.id)
    file_sym.add_defined_symbol(sym1.id)

    file1.library = lib1.id
    lib1.add_defined_file(file1.id)
    g.add_edge(graph_consts.FIL_SYM, file1.id, sym1.id)
    g.add_edge(graph_consts.LIB_SYM, lib1.id, sym1.id)
    g.add_edge(graph_consts.FIL_FIL, file1.id, file_sym.id)
    g.add_edge(graph_consts.LIB_LIB, lib1.id, lib_sym.id)

    file3.library = lib3.id
    lib3.add_defined_file(file3.id)

    file2.library = lib2.id
    lib2.add_defined_file(file2.id)

    g.add_edge(graph_consts.LIB_LIB, lib2.id, lib3.id)
    g.add_edge(graph_consts.LIB_FIL, lib2.id, file3.id)
    g.add_edge(graph_consts.FIL_FIL, file2.id, file3.id)

    lib3.add_dependent_file(file2.id)
    file3.add_dependent_file(file2.id)
    lib3.add_dependent_lib(lib2.id)

    return g


class CustomAssertions:
    """Custom Assertion class for testing node equality"""

    def assertNodeEquals(self, node1, node2):
        if node1.type != node2.type:
            raise AssertionError("Nodes not of same type")

        if node1.type == graph_consts.NODE_LIB:
            if (node1._defined_symbols != node2._defined_symbols or
                    node1._defined_files != node2._defined_files or
                    node1._dependent_libs != node2._dependent_libs or
                    node1._dependent_files != node2._dependent_files or
                    node1._id != node2._id):
                raise AssertionError("Nodes not equal")

        elif node1.type == graph_consts.NODE_SYM:
            if (node1._libs != node2._libs or node1._files != node2._files or
                    node1._dependent_libs != node2._dependent_libs or
                    node1._dependent_files != node2._dependent_files or
                    node1.id != node2.id):
                raise AssertionError("Nodes not equal")

        else:
            if (node1._lib != node2._lib or
                    node1._dependent_libs != node2._dependent_libs or
                    node1._dependent_files != node2._dependent_files or
                    node1.id != node2.id or
                    node1._defined_symbols != node2._defined_symbols):
                raise AssertionError("Nodes not equal")


class TestGraphMethods(unittest.TestCase, CustomAssertions):
    """Unit tests for graph methods"""

    def setUp(self):
        self.g = graph.Graph()

        self.from_node_lib = graph.NodeLib("from_node_lib", "from_node_lib")
        self.to_node_lib = graph.NodeLib("to_node_lib", "to_node_lib")
        self.from_node_file = graph.NodeFile(
            "from_node_file", "from_node_file")
        self.to_node_file = graph.NodeFile("to_node_file", "to_node_file")
        self.from_node_sym = graph.NodeSymbol(
            "from_node_symbol", "from_node_symbol")
        self.to_node_sym = graph.NodeSymbol("to_node_symbol", "to_node_symbol")

        self.g.add_node(self.from_node_lib)
        self.g.add_node(self.to_node_lib)
        self.g.add_node(self.from_node_file)
        self.g.add_node(self.to_node_file)
        self.g.add_node(self.from_node_sym)
        self.g.add_node(self.to_node_sym)

    def test_get_node(self):
        node = graph.NodeLib("test_node", "test_node")
        self.g._nodes = {"test_node": node}

        self.assertEquals(self.g.get_node("test_node"), node)

        self.assertEquals(self.g.get_node("missing_node"), None)

    def test_add_node(self):
        node = graph.NodeLib("test_node", "test_node")
        self.g.add_node(node)

        self.assertEquals(self.g.get_node("test_node"), node)

        self.assertRaises(ValueError, self.g.add_node, node)

        self.assertRaises(TypeError, self.g.add_node, "not a node")

    def test_add_edge_exceptions(self):
        self.assertRaises(TypeError, self.g.add_edge, "NOT A RELATIONSHIP",
                          self.from_node_lib.id, self.to_node_lib.id)

        self.assertRaises(ValueError, self.g.add_edge,
                          graph_consts.LIB_LIB, "not a node", "not a node")

    def test_add_edge_libs(self):
        self.g.add_edge(graph_consts.LIB_LIB, self.from_node_lib.id,
                        self.to_node_lib.id)
        self.g.add_edge(graph_consts.LIB_LIB, self.from_node_lib.id,
                        self.to_node_lib.id)
        self.g.add_edge(graph_consts.LIB_SYM, self.from_node_lib.id,
                        self.to_node_sym.id)
        self.g.add_edge(graph_consts.LIB_FIL, self.from_node_lib.id,
                        self.to_node_file.id)

        self.assertEquals(self.g.edges[graph_consts.LIB_LIB][
            self.from_node_lib.id], set([self.to_node_lib.id]))

        self.assertEquals(self.g.edges[graph_consts.LIB_SYM][
            self.from_node_lib.id], set([self.to_node_sym.id]))

        self.assertEquals(self.g.edges[graph_consts.LIB_FIL][
            self.from_node_lib.id], set([self.to_node_file.id]))

        self.assertEquals(self.to_node_lib.dependent_libs,
                          set([self.from_node_lib.id]))

    def test_add_edge_files(self):
        self.g.add_edge(graph_consts.FIL_FIL, self.from_node_file.id,
                        self.to_node_file.id)
        self.g.add_edge(graph_consts.FIL_SYM, self.from_node_file.id,
                        self.to_node_sym.id)
        self.g.add_edge(graph_consts.FIL_LIB, self.from_node_file.id,
                        self.to_node_lib.id)

        self.assertEquals(self.g.edges[graph_consts.FIL_FIL][
            self.from_node_file.id], set([self.to_node_file.id]))
        self.assertEquals(self.g.edges[graph_consts.FIL_SYM][
            self.from_node_file.id], set([self.to_node_sym.id]))
        self.assertEquals(self.g.edges[graph_consts.FIL_LIB][
            self.from_node_file.id], set([self.to_node_lib.id]))

        self.assertEquals(self.to_node_file.dependent_files,
                          set([self.from_node_file.id]))

    def test_export_to_json(self):
        generated_graph = generate_graph()
        generated_graph.export_to_json("export_test.json")
        generated = open("export_test.json", "r")
        correct = open("test_graph.json", "r")
        self.assertEquals(json.load(generated), json.load(correct))
        generated.close()
        correct.close()

    def test_fromJSON(self):
        graph_fromJSON = graph.Graph("test_graph.json")
        correct_graph = generate_graph()

        for id in graph_fromJSON.nodes.keys():
            # for some reason, neither
            # assertTrue(graph_fromJSON.get_node(id) == correct_graph.get_node(str(id)))
            # nor assertEquals() seem to call the correct eq method here, hence
            # the need for a custom assertion

            self.assertNodeEquals(
                graph_fromJSON.get_node(id), correct_graph.get_node(id))

        self.assertEquals(graph_fromJSON.edges, correct_graph.edges)


if __name__ == '__main__':
    unittest.main()
