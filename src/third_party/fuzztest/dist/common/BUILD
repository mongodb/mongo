# Copyright 2024 Google LLC
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

# The package contains libraries that are common to both FuzzTest and Centipede.

VISIBILITY = ["//visibility:public"]

EXTENDED_API_VISIBILITY = VISIBILITY

PUBLIC_API_VISIBILITY = VISIBILITY

package(default_visibility = VISIBILITY)

licenses(["notice"])

### Files

# Centipede runner needs access to this file, since it has special build
# requirements and needs to compile the file directly with specific flags.
exports_files(
    srcs = ["defs.h"],
    visibility = ["@com_google_fuzztest//centipede:__pkg__"],
)

### Libraries

cc_library(
    name = "bazel",
    srcs = ["bazel.cc"],
    hdrs = ["bazel.h"],
    deps = [
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/time",
    ],
)

cc_library(
    name = "blob_file",
    srcs = ["blob_file.cc"],
    hdrs = ["blob_file.h"],
    defines = select({
        "@com_google_fuzztest//fuzztest:disable_riegeli": ["CENTIPEDE_DISABLE_RIEGELI"],
        "//conditions:default": [],
    }),
    visibility = PUBLIC_API_VISIBILITY,
    deps = [
        ":defs",
        ":hash",
        ":logging",
        ":remote_file",
        ":status_macros",
        "@abseil-cpp//absl/base:core_headers",
        "@abseil-cpp//absl/base:nullability",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings:str_format",
        "@abseil-cpp//absl/strings:string_view",
        "@abseil-cpp//absl/time",
    ] + select({
        "@com_google_fuzztest//fuzztest:disable_riegeli": [],
        "//conditions:default": [
            "@com_google_riegeli//riegeli/base:object",
            "@com_google_riegeli//riegeli/base:types",
            "@com_google_riegeli//riegeli/bytes:reader",
            "@com_google_riegeli//riegeli/bytes:writer",
            "@com_google_riegeli//riegeli/records:record_reader",
            "@com_google_riegeli//riegeli/records:record_writer",
        ],
    }),
)

cc_library(
    name = "defs",
    hdrs = ["defs.h"],
    visibility = PUBLIC_API_VISIBILITY,
    deps = ["@abseil-cpp//absl/types:span"],
)

cc_library(
    name = "hash",
    srcs = ["hash.cc"],
    hdrs = ["hash.h"],
    visibility = EXTENDED_API_VISIBILITY,
    deps = [
        ":defs",
        ":sha1",
    ],
)

cc_library(
    name = "logging",
    hdrs = ["logging.h"],
    deps = [
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:check",
    ],
)

cc_library(
    name = "remote_file",
    srcs = ["remote_file.cc"],
    hdrs = ["remote_file.h"],
    defines = select({
        "@com_google_fuzztest//fuzztest:disable_riegeli": ["CENTIPEDE_DISABLE_RIEGELI"],
        "//conditions:default": [],
    }),
    deps = [
               ":defs",
               ":logging",
               ":status_macros",
               "@abseil-cpp//absl/base:nullability",
               "@abseil-cpp//absl/log:check",
               "@abseil-cpp//absl/status",
               "@abseil-cpp//absl/status:statusor",
               "@abseil-cpp//absl/strings",
           ] + select({
               "//conditions:default": [":remote_file_oss"],
           }) +
           select({
               "@com_google_fuzztest//fuzztest:disable_riegeli": [],
               "//conditions:default": [
                   "@com_google_riegeli//riegeli/bytes:reader",
                   "@com_google_riegeli//riegeli/bytes:writer",
               ],
           }),
)

cc_library(
    name = "remote_file_oss",
    srcs = [
        "remote_file.h",
        "remote_file_oss.cc",
    ],
    defines = select({
        "@com_google_fuzztest//fuzztest:disable_riegeli": ["CENTIPEDE_DISABLE_RIEGELI"],
        "//conditions:default": [],
    }),
    visibility = ["//visibility:private"],
    deps = [
        ":defs",
        ":logging",
        ":status_macros",
        "@abseil-cpp//absl/base:nullability",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
    ] + select({
        "@com_google_fuzztest//fuzztest:disable_riegeli": [],
        "//conditions:default": [
            "@com_google_riegeli//riegeli/bytes:fd_reader",
            "@com_google_riegeli//riegeli/bytes:fd_writer",
            "@com_google_riegeli//riegeli/bytes:reader",
            "@com_google_riegeli//riegeli/bytes:writer",
        ],
    }),
)

cc_library(
    name = "sha1",
    srcs = ["sha1.cc"],
    hdrs = ["sha1.h"],
    visibility = ["//visibility:private"],
    deps = [
        "@abseil-cpp//absl/base:nullability",
    ],
)

cc_library(
    name = "status_macros",
    hdrs = ["status_macros.h"],
    deps = [
        "@abseil-cpp//absl/base:core_headers",
        "@abseil-cpp//absl/log",
    ],
)

cc_library(
    name = "test_util",
    testonly = True,
    srcs = ["test_util.cc"],
    hdrs = ["test_util.h"],
    deps = [
        ":blob_file",
        ":defs",
        ":logging",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/strings:str_format",
        "@googletest//:gtest",
    ],
)

cc_library(
    name = "temp_dir",
    srcs = ["temp_dir.cc"],
    hdrs = ["temp_dir.h"],
    deps = [
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/strings:string_view",
    ],
)

### Tests

cc_test(
    name = "blob_file_test",
    srcs = ["blob_file_test.cc"],
    deps = [
        ":blob_file",
        ":defs",
        ":test_util",
        "@abseil-cpp//absl/status",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "hash_test",
    srcs = ["hash_test.cc"],
    deps = [
        ":hash",
        "@googletest//:gtest_main",
    ],
)

# TODO(b/324462306): Merge this with remote_file_test once the bug is fixed.
cc_library(
    name = "remote_file_test_lib",
    testonly = True,
    srcs = ["remote_file_test.cc"],
    defines = select({
        "//conditions:default": [],
    }),
    deps = [
        ":logging",
        ":remote_file",
        ":test_util",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/time",
        "@googletest//:gtest",
    ] + select({
        "//conditions:default": [],
    }),
    alwayslink = True,
)

cc_test(
    name = "remote_file_test",
    deps = [
        ":remote_file_test_lib",
        "@googletest//:gtest_main",
    ],
)
