# Copyright 2024 The TCMalloc Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

load("//tcmalloc:copts.bzl", "TCMALLOC_DEFAULT_COPTS")

package(default_visibility = ["//visibility:private"])

licenses(["notice"])

cc_library(
    name = "selsan",
    srcs = ["selsan.cc"],
    hdrs = ["selsan.h"],
    copts = TCMALLOC_DEFAULT_COPTS,
    features = [
        "-use_header_modules",  # to avoid flags mismatch errors (b/327555681)
        "-thin_lto",  # workaround thin lto crash (b/330654590)
        "-cfi",  # Requires thin_lto
        "-cfi_diag",  # Requires thin_lto
        "-cfi_dbg",  # Requires thin_lto
    ],
    linkstatic = 1,
    visibility = ["//tcmalloc:__subpackages__"],
    deps = [
        "//tcmalloc/internal:config",
        "//tcmalloc/internal:logging",
        "@com_google_absl//absl/base:core_headers",
    ],
    alwayslink = 1,
)

cc_test(
    name = "report_test",
    srcs = ["report_test.cc"],
    deps = [
        ":selsan",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_googletest//:gtest_main",
    ],
)
