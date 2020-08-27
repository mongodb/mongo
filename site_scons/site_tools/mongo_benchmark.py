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
Pseudo-builders for building and registering benchmarks.
"""
from SCons.Script import Action

from site_scons.mongo import insort_wrapper

def exists(env):
    return True


def build_benchmark(env, target, source, **kwargs):

    bmEnv = env.Clone()
    bmEnv.InjectThirdParty(libraries=["benchmark"])

    if bmEnv.TargetOSIs("windows"):
        bmEnv.Append(LIBS=["ShLwApi.lib"])

    libdeps = kwargs.get("LIBDEPS", bmEnv.get("LIBDEPS", [])).copy()
    insort_wrapper(libdeps, "$BUILD_DIR/mongo/unittest/benchmark_main")

    kwargs["LIBDEPS"] = libdeps
    benchmark_test_components = {"tests", "benchmarks"}
    primary_component = kwargs.get("AIB_COMPONENT", bmEnv.get("AIB_COMPONENT", ""))
    if primary_component and not primary_component.endswith("-benchmark"):
        kwargs["AIB_COMPONENT"] += "-benchmark"
    elif primary_component:
        kwargs["AIB_COMPONENT"] = primary_component
    else:
        kwargs["AIB_COMPONENT"] = "benchmarks"
        benchmark_test_components = {"tests"}

    if "AIB_COMPONENTS_EXTRA" in kwargs:
        benchmark_test_components = set(kwargs["AIB_COMPONENTS_EXTRA"]).union(
            benchmark_test_components
        )

    kwargs["AIB_COMPONENTS_EXTRA"] = benchmark_test_components

    result = bmEnv.Program(target, source, **kwargs)
    bmEnv.RegisterTest("$BENCHMARK_LIST", result[0])
    bmEnv.Alias("$BENCHMARK_ALIAS", result)

    return result


def generate(env):
    env.TestList("$BENCHMARK_LIST", source=[])
    env.AddMethod(build_benchmark, "Benchmark")
    env.Alias("$BENCHMARK_ALIAS", "$BENCHMARK_LIST")
