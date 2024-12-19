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

from collections import defaultdict

from site_scons.mongo import insort_wrapper

BAZEL_BENCHMARK_TAGS = defaultdict(list)
BAZEL_BENCHMARK_TAGS["repl_bm"] = []
BAZEL_BENCHMARK_TAGS["query_bm"] = []
BAZEL_BENCHMARK_TAGS["bsoncolumn_bm"] = []
BAZEL_BENCHMARK_TAGS["first_half_bm"] = []
BAZEL_BENCHMARK_TAGS["second_half_bm"] = []
BAZEL_BENCHMARK_TAGS["wt_storage_bm"] = []
BAZEL_BENCHMARK_TAGS["sharding_bm"] = []


def exists(env):
    return True


def get_bazel_benchmark_tags(env):
    return BAZEL_BENCHMARK_TAGS


def build_benchmark(env, target, source, **kwargs):
    bmEnv = env.Clone()
    bmEnv.InjectThirdParty(libraries=["benchmark"])

    if bmEnv.TargetOSIs("windows"):
        bmEnv.Append(LIBS=["ShLwApi"])

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

    kwargs["AIB_COMPONENTS_EXTRA"] = list(benchmark_test_components)
    if (
        env.GetOption("consolidated-test-bins") == "on"
        and "CONSOLIDATED_TARGET" in kwargs
        and kwargs["CONSOLIDATED_TARGET"]
        and "BAZEL_BENCHMARK_TAG" not in kwargs
    ):
        kwargs["AIB_COMPONENTS_EXTRA"] = ["benchmarks"]
        return bmEnv.AddToConsolidatedTarget(
            target, source, kwargs, "$BENCHMARK_ALIAS", "$BENCHMARK_LIST"
        )

    if "BAZEL_BENCHMARK_TAG" in kwargs:
        kwargs["AIB_COMPONENT"] = kwargs["BAZEL_BENCHMARK_TAG"]
        kwargs["AIB_COMPONENTS_EXTRA"] = []
    if not source:
        result = bmEnv.BazelProgram(target, source, **kwargs)
    else:
        result = bmEnv.Program(target, source, **kwargs)

    if "BAZEL_BENCHMARK_TAG" in kwargs:
        tag = kwargs["BAZEL_BENCHMARK_TAG"]
        BAZEL_BENCHMARK_TAGS[tag] += [target]
        bmEnv.RegisterTest(f"$BUILD_ROOT/{tag}.txt", result[0])
        bmEnv.Alias(f"install-{tag}", result)
    else:
        bmEnv.RegisterTest("$BENCHMARK_LIST", result[0])
        bmEnv.Alias("$BENCHMARK_ALIAS", result)

    return result


def generate(env):
    for tag in BAZEL_BENCHMARK_TAGS:
        env.TestList(f"$BUILD_ROOT/{tag}.txt", source=[])
        env.Alias(f"install-{tag}", f"$BUILD_ROOT/{tag}.txt")
    env.TestList("$BENCHMARK_LIST", source=[])
    env.AddMethod(build_benchmark, "Benchmark")
    env.Alias("$BENCHMARK_ALIAS", "$BENCHMARK_LIST")
    env.AddMethod(get_bazel_benchmark_tags, "get_bazel_benchmark_tags")
