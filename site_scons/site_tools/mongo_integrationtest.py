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

"""
Pseudo-builders for building and registering integration tests.
"""
from SCons.Script import Action

from site_scons.mongo import insort_wrapper

def exists(env):
    return True


def build_cpp_integration_test(env, target, source, **kwargs):
    libdeps = kwargs.get("LIBDEPS", env.get("LIBDEPS", [])).copy()
    insort_wrapper(libdeps, "$BUILD_DIR/mongo/unittest/integration_test_main")

    kwargs["LIBDEPS"] = libdeps
    integration_test_components = {"tests", "integration-tests"}

    primary_component = kwargs.get("AIB_COMPONENT", env.get("AIB_COMPONENT", ""))
    if primary_component and not primary_component.endswith("-test"):
        kwargs["AIB_COMPONENT"] += "-test"
    elif primary_component:
        kwargs["AIB_COMPONENT"] = primary_component
    else:
        kwargs["AIB_COMPONENT"] = "integration-tests"
        integration_test_components = {"tests"}

    if "AIB_COMPONENTS_EXTRA" in kwargs:
        kwargs["AIB_COMPONENTS_EXTRA"] = set(kwargs["AIB_COMPONENTS_EXTRA"]).union(
            integration_test_components
        )
    else:
        kwargs["AIB_COMPONENTS_EXTRA"] = integration_test_components

    result = env.Program(target, source, **kwargs)
    env.RegisterTest("$INTEGRATION_TEST_LIST", result[0])
    env.Alias("$INTEGRATION_TEST_ALIAS", result[0])

    return result


def generate(env):
    env.TestList("$INTEGRATION_TEST_LIST", source=[])
    env.AddMethod(build_cpp_integration_test, "CppIntegrationTest")
    env.Alias("$INTEGRATION_TEST_ALIAS", "$INTEGRATION_TEST_LIST")
