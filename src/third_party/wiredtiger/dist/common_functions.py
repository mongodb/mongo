#!/bin/python3

# Common functions that can be shared across dist/ scripts.
# To call a function `func` from a Python script use:
#     ```
#     import common_functions
#     common_functions.func()
#     ```
# To call a function `func` from the command line or shell script use:
#     ```
#     python3 ./common_functions.py func [optional_arguments_to_function]
#     ```

import inspect
import subprocess
import sys

def last_commit_from_dev():
    # Find the commit from develop at which point the current branch diverged.
    # rev-list will show all commits that are present on our current branch but not on develop, and
    # the oldest of these is our first commit post-divergence. If this commit exists then
    # we can take its parent. If no such commits exist then we're currently on a commit in the
    # develop branch and can use HEAD instead

    earliest_commit = subprocess.run( "git rev-list HEAD...develop | tail -n 1",
        shell=True, capture_output=True, text=True).stdout

    commit_on_dev = f"{earliest_commit}~" if earliest_commit else "HEAD"

    return subprocess.run(f"git rev-parse {commit_on_dev}",
        shell=True, capture_output=True, text=True).stdout.strip()

if __name__ == "__main__":
    # Allow users to execute any function defined in this file via the command line.
    # If a function name is provided and that function is defined in this file then execute
    # it along with any provided arguments.
    functions = dict(inspect.getmembers(sys.modules[__name__], predicate=inspect.isfunction))

    if len(sys.argv) > 1:
        name, *args = sys.argv[1:]

        if name in functions:
            print(functions[name](*args))
            sys.exit(0)
        else:
            print(f"Function name '{name}' not recognised!")
            print(f"Available functions are {list(functions.keys())}")
    else:
        print("Usage: `python3 ./common_functions.py function_name [function_args...]`")

    sys.exit(1)
