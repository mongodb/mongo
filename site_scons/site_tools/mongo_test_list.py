# Copyright 2019 MongoDB Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Pseudo-builders for building test lists for Resmoke"""

import SCons
from collections import defaultdict

TEST_REGISTRY = defaultdict(list)


def register_test(env, file, test):
    """Register test into the dictionary of tests for file_name"""
    test_path = test.path
    if getattr(test.attributes, "AIB_INSTALL_ACTIONS", []):
        test_path = getattr(test.attributes, "AIB_INSTALL_ACTIONS")[0].path

    if SCons.Util.is_String(file):
        file = env.File(file)

    env.Depends(file, test)
    file_name = file.path
    TEST_REGISTRY[file_name].append(test_path)
    env.GenerateTestExecutionAliases(test)


def test_list_builder_action(env, target, source):
    """Build a test list used by resmoke.py to execute binary tests."""
    if SCons.Util.is_String(target[0]):
        filename = env.subst(target[0])
    else:
        filename = target[0].path

    source = [env.subst(s) if SCons.Util.is_String(s) else s.path for s in source]

    with open(filename, "w") as ofile:
        tests = TEST_REGISTRY[filename]
        if source:
            tests.extend(source)

        for s in tests:
            ofile.write("{}\n".format(str(s)))


TEST_LIST_BUILDER = SCons.Builder.Builder(
    action=SCons.Action.FunctionAction(
        test_list_builder_action, {"cmdstr": "Generating $TARGETS"},
    )
)


def exists(env):
    return True


def generate(env):
    env["MONGO_TEST_REGISTRY"] = TEST_REGISTRY
    env.Append(BUILDERS={"TestList": TEST_LIST_BUILDER})
    env.AddMethod(register_test, "RegisterTest")
