# Copyright 2022 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Tools.

package(default_visibility = ["//visibility:public"])

licenses(["notice"])

sh_binary(
    name = "minimizer",
    srcs = ["minimizer.sh"],
)

cc_binary(
    name = "grammar_domain_code_generator",
    srcs = ["grammar_domain_code_generator.cc"],
    deps = [
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/flags:parse",
        "@com_google_fuzztest//fuzztest/internal:logging",
        "@com_google_fuzztest//grammar_codegen:code_generation",
    ],
)
