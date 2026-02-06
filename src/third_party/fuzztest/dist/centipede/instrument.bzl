# Copyright 2023 The Centipede Authors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Utilities for applying or removing instrumentation to binary targets."""

def _strip_instrumentation_transition_impl(settings, _):
    return {
        "//command_line_option:compilation_mode": "opt",
        "//command_line_option:compiler": None,
        "//command_line_option:copt": [],
        "//command_line_option:custom_malloc": None,
        "//command_line_option:dynamic_mode": "default",
        "//command_line_option:features": [
            feature
            for feature in settings["//command_line_option:features"]
            if feature not in ["asan", "tsan", "msan"]
        ],
        "//command_line_option:linkopt": [],
        "//command_line_option:per_file_copt": [],
        "//command_line_option:strip": "never",
    }

strip_instrumentation_transition = transition(
    implementation = _strip_instrumentation_transition_impl,
    inputs = [
        "//command_line_option:features",
    ],
    outputs = [
        "//command_line_option:compilation_mode",
        "//command_line_option:compiler",
        "//command_line_option:copt",
        "//command_line_option:custom_malloc",
        "//command_line_option:dynamic_mode",
        "//command_line_option:features",
        "//command_line_option:linkopt",
        "//command_line_option:per_file_copt",
        "//command_line_option:strip",
    ],
)

def _cc_uninstrumented_binary_impl(ctx):
    output_file = ctx.actions.declare_file(ctx.label.name)
    ctx.actions.symlink(
        output = output_file,
        target_file = ctx.executable.binary,
        is_executable = True,
    )
    runfiles = ctx.runfiles()
    runfiles = runfiles.merge(ctx.attr.binary[0][DefaultInfo].default_runfiles)
    return [
        DefaultInfo(
            executable = output_file,
            runfiles = runfiles,
        ),
    ]

cc_uninstrumented_binary = rule(
    implementation = _cc_uninstrumented_binary_impl,
    doc = """
Removes all known Centipede instrumentation that might have been applied to a
target cc_binary.
""",
    attrs = {
        "binary": attr.label(
            doc = "A cc_binary target to apply the instrumentation to.",
            executable = True,
            cfg = strip_instrumentation_transition,
            mandatory = True,
        ),
        "_allowlist_function_transition": attr.label(
            default = "@bazel_tools//tools/allowlists/function_transition_allowlist",
        ),
    },
    executable = True,
)
