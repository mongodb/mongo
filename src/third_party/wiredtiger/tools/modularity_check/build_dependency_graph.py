import dataclasses as dc
from collections import defaultdict, Counter
from typing import List

from parse_wt_ast import File

import networkx as nx

# If we can't determine the module something links to track it in the ambiguous node.
# These can be reviewed manually
AMBIG_NODE = "Ambiguous linking or parsing failed"

# A link in the dependency graph from module A to module B.
# The class contains details of why the link exists - what functions, macros, struct accesses cause the dependency
@dc.dataclass
class Link:
    func_calls: Counter= dc.field(default_factory=lambda: Counter())
    types_used: Counter= dc.field(default_factory=lambda: Counter())
    # struct_accesses format: {struct: {field: num_accesses}}
    struct_accesses: defaultdict = dc.field(default_factory=lambda: defaultdict(Counter)) 

    def print_struct_accesses(self):
        if len(self.struct_accesses) == 0:
            return
 
        print("    struct_accesses:")
        for struct, fields in sorted(self.struct_accesses.items()):
            print(f"        {struct}")
            for field in sorted(fields):
                print(f"            {field}: {fields[field]}")
            print()

    def print_func_calls(self):
        if len(self.func_calls) == 0:
            return
        print("    function_calls:")
        for func, num_calls in sorted(self.func_calls.items()):
            print(f"        {func}: {num_calls}")
        print()

    def print_type_uses(self):
        if len(self.types_used) == 0:
            return
        print(f"    types_used: {sorted(self.types_used)}")


def incr_edge_struct_access(graph, calling_module, dest_module, field, struct):
    if calling_module != dest_module: # No need to add reference to self
        if not graph.has_edge(calling_module, dest_module):
            graph.add_edge(calling_module, dest_module)
            graph[calling_module][dest_module]['link_data'] = Link()
        graph[calling_module][dest_module]['link_data'].struct_accesses[struct][field] += 1

def incr_edge_func_call(graph, calling_module, dest_module, func_call):
    if calling_module != dest_module: # No need to add reference to self
        if not graph.has_edge(calling_module, dest_module):
            graph.add_edge(calling_module, dest_module)
            graph[calling_module][dest_module]['link_data'] = Link()
        graph[calling_module][dest_module]['link_data'].func_calls[func_call] += 1

def incr_edge_type_use(graph, calling_module, dest_module, type_use):
    if calling_module != dest_module: # No need to add reference to self
        if not graph.has_edge(calling_module, dest_module):
            graph.add_edge(calling_module, dest_module)
            graph[calling_module][dest_module]['link_data'] = Link()
        graph[calling_module][dest_module]['link_data'].types_used[type_use] += 1


# Walk all files and create a module-to-module dependency graph.
# We'll stick the reason for the dependency (function call, struct access, 
# use of type) in the edge metadata
def build_graph(parsed_files: List[File]):

    field_to_struct_map = defaultdict(set)
    struct_to_module_map = defaultdict(set)
    type_to_module_map = defaultdict(set)
    func_to_module_map = defaultdict(set)

    # Build up the reverse mappings
    for file in parsed_files:
        for func in file.functions:
            func_to_module_map[func.name].add(file.module)

        for struct in file.structs:
            struct_to_module_map[struct.name].add(file.module)
            for field in struct.fields:
                field_to_struct_map[field].add(struct.name)

        for type in file.types_defined:
            type_to_module_map[type].add(file.module)

    graph = nx.DiGraph()
    ambiguous_fields = set()

    graph.add_node(AMBIG_NODE)

    for file in parsed_files:
        calling_module = file.module
        graph.add_node(file.module)

        for func in file.functions:
            for field_access in func.fields_accessed:
                structs_with_field = field_to_struct_map[field_access]
                if len(structs_with_field) == 1:
                    dest_struct = list(structs_with_field)[0]
                    dest_modules = struct_to_module_map[dest_struct]
                    if len(dest_modules) == 1:
                        dest_module = list(dest_modules)[0]
                        incr_edge_struct_access(graph, calling_module, dest_module, field_access, dest_struct)
                    else:
                        incr_edge_struct_access(graph, calling_module, AMBIG_NODE, field_access, dest_struct)
                        ambiguous_fields.add(field_access)
                else:
                    unknown_struct = "These fields belong to multiple structs! Please check manually"
                    incr_edge_struct_access(graph, calling_module, AMBIG_NODE, field_access, unknown_struct)
                    ambiguous_fields.add(field_access)

            for called_func in func.functions_called:
                called_modules = func_to_module_map[called_func]
                if len(called_modules) == 1:
                    dest_module = list(called_modules)[0]
                    incr_edge_func_call(graph, calling_module, dest_module, called_func)
                else:
                    incr_edge_func_call(graph, calling_module, AMBIG_NODE, called_func)

            for type_use in func.types_used:
                type_modules = type_to_module_map[type_use]
                if len(type_modules) == 1:
                    dest_module = list(type_modules)[0]
                    incr_edge_type_use(graph, calling_module, dest_module, type_use)
                else:
                    incr_edge_type_use(graph, calling_module, AMBIG_NODE, type_use)

    return graph, ambiguous_fields
