"""Unit tests for buildscripts/resmokelib/suite_hierarchy.py."""

import unittest

from buildscripts.resmokelib.suite_hierarchy import (
    compute_ancestors,
    compute_dag,
    compute_minimal_test_set,
)

tests_in_suite = {
    "A": set(["t1", "t2", "t3"]),
    "B": set(["t1", "t2", "t3", "t4"]),
    "C": set(["t1", "t2", "t3", "t5"]),
    "D": set(["t2", "t3", "t5", "t6"]),
    "E": set(["t1", "t2", "t3", "t7"]),
    "P": set(
        [
            "t1",
        ]
    ),
    "Q": set(["t1", "t7"]),
}

# Graph 1:
# A       P
# |       |
# |       V
# |       Q
# |       |
# V       |
# B <-----+
# |
# +-------+
# |       |
# V       V
# C       D
# |
# V
# E
#
# Different equivalent ways of representing Graph 1
graph1 = [
    {"A": {"B": {"C": {"E": {}}, "D": {}}}, "P": {"Q": {"B": {}}}},
    {
        "A": {
            "B": {},
        },
        "P": {"Q": {"B": {}}},
        "B": {"C": {"E": {}}, "D": {}},
    },
    {"A": {"B": {"C": {"E": {}}, "D": {}}}, "P": {"Q": {}}, "Q": {"B": {}}},
    {
        "A": {"B": {}},
        "B": {"C": {}, "D": {}},
        "C": {"E": {}},
        "D": {},
        "P": {"Q": {}},
        "Q": {"B": {}},
    },
]

correct_dag1 = {
    "A": {"parents": set(), "children": set(["B"])},
    "B": {"parents": set(["A", "Q"]), "children": set(["C", "D"])},
    "C": {"parents": set(["B"]), "children": set(["E"])},
    "D": {"parents": set(["B"]), "children": set()},
    "E": {"parents": set(["C"]), "children": set()},
    "P": {"parents": set(), "children": set(["Q"])},
    "Q": {"parents": set(["P"]), "children": set(["B"])},
}

correct_ancestors1 = [
    ("A", set()),
    ("B", set(["A", "P", "Q"])),
    ("C", set(["A", "P", "Q", "B"])),
    ("D", set(["A", "P", "Q", "B"])),
    ("E", set(["A", "P", "Q", "B", "C"])),
    ("P", set()),
    ("Q", set(["P"])),
]

# Minimal set for graph 1
correct_minimal_set1 = {
    "A": set(["t1", "t2", "t3"]),
    "B": set(["t4"]),
    "C": set(["t5"]),
    "D": set(["t5", "t6"]),
    "E": set(),
    "P": set(
        [
            "t1",
        ]
    ),
    "Q": set(["t7"]),
}

# Graph 2:
# A--+    P (disconnected)
# |  |
# |  |
# |  |    Q (disconnected)
# |  |
# V  |
# B  |
# |  |
# |  |
# |  |
# V  |
# C <+
#
# Different equivalent ways of representing Graph 2
graph2 = [
    {"A": {"B": {"C": {}}, "C": {}}, "P": {}, "Q": {}},
    {"B": {"C": {}}, "A": {"B": {}, "C": {}}, "P": {}, "Q": {}},
]

correct_dag2 = {
    "A": {"parents": set(), "children": set(["B", "C"])},
    "B": {"parents": set(["A"]), "children": set(["C"])},
    "C": {"parents": set(["A", "B"]), "children": set()},
    "P": {"parents": set(), "children": set()},
    "Q": {"parents": set(), "children": set()},
}

correct_ancestors2 = [
    ("A", set()),
    ("B", set(["A"])),
    ("C", set(["A", "B"])),
    ("P", set()),
    ("Q", set()),
]

# Minimal set for graph 2
correct_minimal_set2 = {
    "A": set(["t1", "t2", "t3"]),
    "B": set(["t4"]),
    "C": set(["t5"]),
    "P": set(["t1"]),
    "Q": set(["t1", "t7"]),
}


class TestSuiteHierarchy(unittest.TestCase):
    def test_compute_dag_empty(self):
        correct_dag = {}
        self.assertEqual(correct_dag, compute_dag({}))

    def test_compute_dag_graph1(self):
        for graph in graph1:
            dag = compute_dag(graph)
            self.assertEqual(
                correct_dag1,
                dag,
                f"Expected: \n{correct_dag1}\nbut received\n{dag}.\nTest case:\n{graph}",
            )

    def test_compute_dag_graph2(self):
        for graph in graph2:
            dag = compute_dag(graph)
            self.assertEqual(
                correct_dag2,
                dag,
                f"Expected: \n{correct_dag2}\nbut received\n{dag}.\nTest case:\n{graph}",
            )

    def test_compute_ancestors_graph1(self):
        for node, answer in correct_ancestors1:
            ancestors = compute_ancestors(node, correct_dag1)
            self.assertEqual(answer, ancestors, f"Expected\n{answer}\nbut received\n{ancestors}")

    def test_compute_ancestors_graph2(self):
        for node, answer in correct_ancestors2:
            ancestors = compute_ancestors(node, correct_dag2)
            self.assertEqual(answer, ancestors, f"Expected\n{answer}\nbut received\n{ancestors}")

    def test_compute_minimal_test_set1(self):
        for node, tests in correct_minimal_set1.items():
            computed_tests = compute_minimal_test_set(node, correct_dag1, tests_in_suite)
            self.assertEqual(
                tests,
                computed_tests,
                f"On node {node}\nExpected\n{tests}\nbut received\n{computed_tests}",
            )

    def test_compute_minimal_test_set2(self):
        for node, tests in correct_minimal_set2.items():
            computed_tests = compute_minimal_test_set(node, correct_dag2, tests_in_suite)
            self.assertEqual(
                tests,
                computed_tests,
                f"On node {node}\nExpected\n{tests}\nbut received\n{computed_tests}",
            )

    def test_union_of_minimal_equals_union_of_full(self):
        # Test that the union(minimal set of each ancestor) is equal
        # to the union(full test set of each ancestor)
        for node, ancestors in correct_ancestors1:
            min_set_union = set()
            full_union = set()
            for ancestor in ancestors:
                min_set_union = min_set_union.union(
                    compute_minimal_test_set(ancestor, correct_dag1, tests_in_suite)
                )
                full_union = full_union.union(tests_in_suite[ancestor])
            self.assertEqual(
                min_set_union,
                full_union,
                f"Min set union\n{min_set_union}\n != full union\n{full_union}",
            )

        for node, ancestors in correct_ancestors2:
            min_set_union = set()
            full_union = set()
            for ancestor in ancestors:
                min_set_union = min_set_union.union(
                    compute_minimal_test_set(ancestor, correct_dag2, tests_in_suite)
                )
                full_union = full_union.union(tests_in_suite[ancestor])
            self.assertEqual(
                min_set_union,
                full_union,
                f"Min set union\n{min_set_union}\n != full union\n{full_union}",
            )
