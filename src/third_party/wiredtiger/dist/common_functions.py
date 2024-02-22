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

import inspect, os, subprocess, sys


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


def is_fast_mode(fast = None):
    fast = fast or "-F" in sys.argv[1:] or os.environ.get('WT_FAST', '') != ''
    if fast:
        os.environ["WT_FAST"] = "1"
    return fast


def files_changed_from_commit(prefix, commit = None):
    # `git diff --name-only` returns file names relative to the root of the repository.
    # If we want to use these file names from another location, we need to prefix them.
    # NOTE: The prefix must exactly match how the script refers to the repository root!
    # For example, if the script searches for files from the root via `find src -name *.c`,
    # then the prefix must be "".
    # If the script searches for files from the `dist` dir via `find ../ -name *.c`,
    # then the prefix must be "../".
    # Other ways to refer the file names (like "../dist/..") will not work.
    if commit is None:
        commit = last_commit_from_dev()
    # Find the files that have changed since the last commit
    return [prefix+f for f in subprocess.run(f"git diff --name-only {commit}",
        shell=True, capture_output=True, text=True).stdout.strip().splitlines()]


def filter_changed_files(in_generator, prefix, commit = None):
    # prefix is for when the file names are not relative to the root of the repository.
    # NOTE: See the comment in `files_changed_from_commit` for more information on the prefix.
    changed_files = set(files_changed_from_commit(prefix=prefix, commit=commit))
    for file in in_generator:
        if file in changed_files:
            yield file


def filter_if_fast(in_generator, prefix, fast = None):
    # Prefix is for when the file names are not relative to the root of the repository.
    # NOTE: See the comment in `files_changed_from_commit` for more information on the prefix.
    if is_fast_mode(fast):
        changed_files = filter_changed_files(in_generator, prefix=prefix)
    else:
        changed_files = in_generator

    for file in changed_files:
        yield file


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
