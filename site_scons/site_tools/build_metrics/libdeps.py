import json
import os
import sys

import networkx

from .protocol import BuildMetricsCollector

# libdeps analyzer does not assume the root build directory, so we need to add its own root to the path
dir_path = os.path.dirname(os.path.realpath(__file__))
sys.path.insert(0, os.path.join(dir_path, "..", "..", "..", "buildscripts", "libdeps"))

from buildscripts.libdeps.libdeps.analyzer import (
    GaJsonPrinter,
    LibdepsGraphAnalysis,
    counter_factory,
)
from buildscripts.libdeps.libdeps.graph import CountTypes, LibdepsGraph

_ALLOWED_KEYS = set(
    [
        "NODE",
        "EDGE",
        "DIR_EDGE",
        "TRANS_EDGE",
        "DIR_PUB_EDGE",
        "PUB_EDGE",
        "PRIV_EDGE",
        "IF_EDGE",
        "PROG",
        "LIB",
    ]
)


class LibdepsCollector(BuildMetricsCollector):
    def __init__(self, env):
        self._env = env

    def get_name(self):
        return "LibdepsCollector"

    @staticmethod
    def _libdeps(graph_file):
        libdeps_graph = LibdepsGraph(graph=networkx.read_graphml(graph_file))

        if libdeps_graph.graph["graph_schema_version"] == 1:
            libdeps_graph = networkx.reverse_view(libdeps_graph)

        return GaJsonPrinter(
            LibdepsGraphAnalysis(counter_factory(libdeps_graph, CountTypes.ALL.name))
        ).get_json()

    @staticmethod
    def _finalize(libdeps_graph_file):
        out = {}
        for key, value in json.loads(LibdepsCollector._libdeps(libdeps_graph_file)).items():
            if key in _ALLOWED_KEYS:
                out[key] = value
        return out

    def finalize(self):
        libdeps_graph_file = self._env.get("LIBDEPS_GRAPH_FILE")
        out = {}
        if libdeps_graph_file is not None and os.path.exists(libdeps_graph_file.path):
            out = self._finalize(libdeps_graph_file.path)
        else:
            print(
                f"WARNING: libdeps graph file '{libdeps_graph_file}' could not be found. Skipping libdeps metrics"
            )
        return "libdeps_metrics", out
