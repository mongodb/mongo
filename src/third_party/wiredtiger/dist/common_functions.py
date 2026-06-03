#!/bin/python3
#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.

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
    # Find the most recent common ancestor between the current branch and origin/develop.
    # Using git merge-base correctly handles branches that have merged develop multiple
    # times: it returns the latest develop commit reachable from HEAD, so that
    # filter_if_fast only sees files changed by the author's own commits rather than
    # every file touched by develop since the branch was first created.
    return subprocess.run("git merge-base origin/develop HEAD",
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
