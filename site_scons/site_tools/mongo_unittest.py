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
"""Pseudo-builders for building and registering unit tests."""

import json
import os

from buildscripts.unittest_grouper import find_group
from site_scons.mongo import insort_wrapper


def exists(env):
    return True


TEST_GROUPS = []


def build_cpp_unit_test(env, target, source, **kwargs):
    if not isinstance(target, list):
        target = [target]

    for t in target:
        if not t.endswith("_test"):
            env.ConfError(f"CppUnitTest target `{t}' does not end in `_test'")

    scons_node = env.File(os.path.join(os.getcwd(), str(target[0])))
    root_path = scons_node.abspath.replace("\\", "/").replace(
        env.Dir("#").abspath.replace("\\", "/") + "/", ""
    )
    if root_path.startswith(env.Dir("$BUILD_DIR").path.replace("\\", "/")):
        root_path = "src" + root_path[len(env.Dir("$BUILD_DIR").path.replace("\\", "/")) :]
    root_path = root_path.replace("\\", "/")

    test_group = list(json.loads(find_group([root_path])).keys())[0]

    if test_group not in TEST_GROUPS:
        TEST_GROUPS.append(test_group)
        env.TestList(f"$BUILD_ROOT/{test_group}_quarter_unittests.txt", source=[])
        env.Alias(
            f"install-{test_group}-quarter-unittests",
            f"$BUILD_ROOT/{test_group}_quarter_unittests.txt",
        )

    if not kwargs.get("UNITTEST_HAS_CUSTOM_MAINLINE", False):
        libdeps = kwargs.get("LIBDEPS", env.get("LIBDEPS", [])).copy()
        insort_wrapper(libdeps, "$BUILD_DIR/mongo/unittest/unittest_main")
        kwargs["LIBDEPS"] = libdeps

    unit_test_components = {"tests", "unittests"}
    primary_component = kwargs.get("AIB_COMPONENT", env.get("AIB_COMPONENT", ""))
    if primary_component and not primary_component.endswith("-test"):
        kwargs["AIB_COMPONENT"] = primary_component + "-test"
    elif primary_component:
        kwargs["AIB_COMPONENT"] = primary_component
    else:
        kwargs["AIB_COMPONENT"] = f"{test_group}-quarter-unittests"

    if "AIB_COMPONENTS_EXTRA" in kwargs:
        kwargs["AIB_COMPONENTS_EXTRA"] = set(kwargs["AIB_COMPONENTS_EXTRA"]).union(
            unit_test_components
        )
    else:
        kwargs["AIB_COMPONENTS_EXTRA"] = list(unit_test_components)

    if "PROVE_ALIASES" in kwargs:
        for alias in kwargs.get("PROVE_ALIASES"):
            env.Alias(f"prove-{alias}", env.Alias(f"prove-{target[0]}"))

    if not source:
        result = env.BazelProgram(target, source, **kwargs)
    else:
        result = env.Program(target, source, **kwargs)

    env.RegisterTest("$UNITTEST_LIST", result[0])
    env.Alias("$UNITTEST_ALIAS", result[0])

    env.RegisterTest(
        f"$BUILD_ROOT/{test_group}_quarter_unittests.txt", result[0], generate_alias=False
    )
    env.Alias(f"install-{test_group}-quarter-unittests", result[0])

    return result


def generate(env):
    env.TestList("$UNITTEST_LIST", source=[])
    env.AddMethod(build_cpp_unit_test, "CppUnitTest")
    env.Alias("$UNITTEST_ALIAS", "$UNITTEST_LIST")
