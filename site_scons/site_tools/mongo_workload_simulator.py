# Copyright 2023 MongoDB Inc.
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
"""Pseudo-builders for building workload simulators."""

from site_scons.mongo import insort_wrapper


def exists(env):
    return True


def build_workload_simulator(env, target, source, **kwargs):
    if not isinstance(target, list):
        target = [target]

    for t in target:
        if not t.endswith("_simulator"):
            env.ConfError(f"WorkloadSimulator target `{t}' does not end in `_simulator'")

    libdeps = kwargs.get("LIBDEPS", env.get("LIBDEPS", [])).copy()
    insort_wrapper(libdeps, "$BUILD_DIR/mongo/tools/workload_simulation/simulator_main")
    kwargs["LIBDEPS"] = libdeps

    if not source:
        result = env.BazelProgram(target, source, **kwargs)
    else:
        result = env.Program(target, source, **kwargs)

    return result


def generate(env):
    env.AddMethod(build_workload_simulator, "WorkloadSimulator")
