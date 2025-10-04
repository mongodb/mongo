# Modularity checker tool

Tool to review the modularity of components in WiredTiger.

> [!WARNING]  
> This tool is not perfect. Please validate all results.

## Getting started:
```sh
virtualenv venv
(venv) pip install -r requirement.txt
```

## Using the tool:
```sh
./modularity_check.py --help
./modularity_check.py who_uses log                           # Report all the users of the log module
./modularity_check.py who_is_used_by evict                   # Report all other modules used by the log module
./modularity_check.py list_cycles conn                       # Report all dependency cycles up to length 3 that include conn/
./modularity_check.py explain_cycle "['log', 'meta', 'txn']" # Explain why the provided dependency cycle exists
./modularity_check.py privacy_report txn                     # Report which structs and fields in a module are private
./modularity_check.py generate_dependency_file               # Generate a text representation of the module dependency graph.
```

## Important notes before using this tool:
- This script is not perfect! Please see the known issues below
- `header_mapping.py` takes an opinionated stance on which files in `src/include/` belong to which modules. Some of these may be incorrect.
- All subfolders in `src/checksum/` are treated as a single `checksum` module
- The `src/os_*` folders are treated as a single `os_layer` module
- Very common functions like `WT_RET` are ignored by this script as they add noise but no signal. The list of ignored functions/macros can be found in `parse_wt_ast.py::filter_common_calls()`

## Known issues:
- Macros are not part of the C grammar so tree-sitter struggles to deal with them. It's important to call out even though macros aren't part of the grammar the `tree-sitter-c` library does a very good job at working around this. Nonetheless this means the script does some manual pre-processing work on files. See `parse_wt_ast.py::preprocess_file()` for this manual work.
- The current script parses the AST so there's no semantic data associated with fields. As such field accesses are mapped to their owning struct by their unique names. If a field is defined in two structs it is ambiguous and can't be mapped. When this happens it is instead linked to an `Ambiguous linking or parsing failed` node in the graph to be reported to the user.
- There are some know incorrect parsing results, for example `who_is_used_by log` reports that log calls the function `(*func)`, which is actually part of a function_pointer argument passed used by `__wt_log_scan`. This may be fixed in the future, but for now is an acceptable inaccuracy in the tool.

## How the tool works:
`modularity_check.py` is the entry point to the tool. A list of example uses can be found above under `Using the tool`  
Once the argument are processed `parse_wt_ast.py` walks all files in the `src/` directory and uses tree-sitter to parse the codes Abstract Syntax Tree. This AST is then processed to determine in which files a struct or function is defined and/or used.  

Using this data `build_dependency_graph.py` uses `networkx` to build a directed graph of module dependencies. If code in module A accesses code in module B, then A depends on B. These links contain information on which struct, type, macro, or function was used to create the dependency.

Finally `query_dependency_graph.py` polls the dependency graph to return results to the user.
