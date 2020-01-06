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

"""Pseudo-builders for building and registering unit tests."""
from SCons.Script import Action


def register_unit_test(env, test):
    """
    Kept around for compatibility with non-hygienic builds. The only callers of
    this should be the intel_readtest_wrapper SConscript. All original callers
    have been updated to use UNITTEST_HAS_CUSTOM_MAINLINE.
    """
    env.RegisterTest("$UNITTEST_LIST", test)
    if not env.get("AUTO_INSTALL_ENABLED", False):
        env.Alias("$UNITTEST_ALIAS", test)


def exists(env):
    return True


def build_cpp_unit_test(env, target, source, **kwargs):
    if not kwargs.get("UNITTEST_HAS_CUSTOM_MAINLINE", False):
        libdeps = kwargs.get("LIBDEPS", [])
        libdeps.append("$BUILD_DIR/mongo/unittest/unittest_main")
        kwargs["LIBDEPS"] = libdeps

    unit_test_components = {"tests", "unittests"}
    primary_component = kwargs.get("AIB_COMPONENT", env.get("AIB_COMPONENT", ""))
    if primary_component and not primary_component.endswith("-test"):
        kwargs["AIB_COMPONENT"] = primary_component + "-test"
    elif primary_component:
        kwargs["AIB_COMPONENT"] = primary_component
    else:
        kwargs["AIB_COMPONENT"] = "unittests"
        unit_test_components = {"tests"}

    if "AIB_COMPONENTS_EXTRA" in kwargs:
        kwargs["AIB_COMPONENTS_EXTRA"] = set(kwargs["AIB_COMPONENTS_EXTRA"]).union(
            unit_test_components
        )
    else:
        kwargs["AIB_COMPONENTS_EXTRA"] = unit_test_components

    result = env.Program(target, source, **kwargs)
    env.RegisterTest("$UNITTEST_LIST", result[0])
    env.Alias("$UNITTEST_ALIAS", result[0])

    return result


def generate(env):
    env.TestList("$UNITTEST_LIST", source=[])
    env.AddMethod(build_cpp_unit_test, "CppUnitTest")
    env.AddMethod(register_unit_test, "RegisterUnitTest")
    env.Alias("$UNITTEST_ALIAS", "$UNITTEST_LIST")
