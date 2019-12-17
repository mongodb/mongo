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

"""
Pseudo-builders for building and registering benchmarks.
"""
from SCons.Script import Action


def exists(env):
    return True


def build_benchmark(env, target, source, **kwargs):

    bmEnv = env.Clone()
    bmEnv.InjectThirdParty(libraries=["benchmark"])

    if bmEnv.TargetOSIs("windows"):
        bmEnv.Append(LIBS=["ShLwApi.lib"])

    libdeps = kwargs.get("LIBDEPS", [])
    libdeps.append("$BUILD_DIR/mongo/unittest/benchmark_main")

    kwargs["LIBDEPS"] = libdeps
    benchmark_test_components = {"tests", "benchmarks"}
    if "AIB_COMPONENT" in kwargs and not kwargs["AIB_COMPONENT"].endswith("-benchmark"):
        kwargs["AIB_COMPONENT"] += "-benchmark"

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
