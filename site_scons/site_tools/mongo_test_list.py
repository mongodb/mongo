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
"""Pseudo-builders for building test lists for Resmoke"""

from collections import defaultdict

import SCons

TEST_REGISTRY = defaultdict(list)


def register_test(env, file, test, generate_alias=True):
    """Register test into the dictionary of tests for file_name"""
    test_path = test
    if env.get("AUTO_INSTALL_ENABLED", False) and env.GetAutoInstalledFiles(test):
        test_path = env.GetAutoInstalledFiles(test)[0]

    if SCons.Util.is_String(file):
        file = env.File(file)

    env.Depends(file, test_path)
    file_name = file.path
    TEST_REGISTRY[file_name].append(test_path)
    if generate_alias:
        env.GenerateTestExecutionAliases(test)


def test_list_builder_action(env, target, source):
    """Build a test list used by resmoke.py to execute binary tests."""
    if SCons.Util.is_String(target[0]):
        filename = env.subst(target[0])
    else:
        filename = target[0].path

    source = [env.File(s).path if SCons.Util.is_String(s) else s.path for s in source]

    with open(filename, "w") as ofile:
        tests = TEST_REGISTRY[filename]
        if source:
            tests.extend(source)

        for s in tests:
            ofile.write("{}\n".format(str(s)))


TEST_LIST_BUILDER = SCons.Builder.Builder(
    action=SCons.Action.FunctionAction(
        test_list_builder_action,
        {"cmdstr": "Generating $TARGETS"},
    )
)


def exists(env):
    return True


def generate(env):
    env["MONGO_TEST_REGISTRY"] = TEST_REGISTRY
    env.Append(BUILDERS={"TestList": TEST_LIST_BUILDER})
    env.AddMethod(register_test, "RegisterTest")
