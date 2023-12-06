#
# Copyright 2019 The Abseil Authors.
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
#

workspace(name = "com_google_absl")

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

# GoogleTest/GoogleMock framework. Used by most unit-tests.
http_archive(
    name = "com_google_googletest",  # 2023-08-02T16:45:10Z
    sha256 = "1f357c27ca988c3f7c6b4bf68a9395005ac6761f034046e9dde0896e3aba00e4",
    strip_prefix = "googletest-1.14.0",
    # Keep this URL in sync with ABSL_GOOGLETEST_COMMIT in ci/cmake_common.sh.
    urls = ["https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip"],
)

# RE2 (the regular expression library used by GoogleTest)
http_archive(
    name = "com_googlesource_code_re2",  # 2023-03-17T11:36:51Z
    sha256 = "cb8b5312a65f2598954545a76e8bce913f35fbb3a21a5c88797a4448e9f9b9d9",
    strip_prefix = "re2-578843a516fd1da7084ae46209a75f3613b6065e",
    urls = ["https://github.com/google/re2/archive/578843a516fd1da7084ae46209a75f3613b6065e.zip"],
)

# Google benchmark.
http_archive(
    name = "com_github_google_benchmark",  # 2023-08-01T07:47:09Z
    sha256 = "db1e39ee71dc38aa7e57ed007f2c8b3bb59e13656435974781a9dc0617d75cc9",
    strip_prefix = "benchmark-02a354f3f323ae8256948e1dc77ddcb1dfc297da",
    urls = ["https://github.com/google/benchmark/archive/02a354f3f323ae8256948e1dc77ddcb1dfc297da.zip"],
)

# Bazel Skylib.
http_archive(
    name = "bazel_skylib",  # 2023-05-31T19:24:07Z
    sha256 = "08c0386f45821ce246bbbf77503c973246ed6ee5c3463e41efc197fa9bc3a7f4",
    strip_prefix = "bazel-skylib-288731ef9f7f688932bd50e704a91a45ec185f9b",
    urls = ["https://github.com/bazelbuild/bazel-skylib/archive/288731ef9f7f688932bd50e704a91a45ec185f9b.zip"],
)

# Bazel platform rules.
http_archive(
    name = "platforms",  # 2023-07-28T19:44:27Z
    sha256 = "40eb313613ff00a5c03eed20aba58890046f4d38dec7344f00bb9a8867853526",
    strip_prefix = "platforms-4ad40ef271da8176d4fc0194d2089b8a76e19d7b",
    urls = ["https://github.com/bazelbuild/platforms/archive/4ad40ef271da8176d4fc0194d2089b8a76e19d7b.zip"],
)
