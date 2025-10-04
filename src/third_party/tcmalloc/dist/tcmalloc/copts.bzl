# Copyright 2019 The TCMalloc Authors
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

"""This package provides default compiler warning flags for the OSS release"""

TCMALLOC_LLVM_FLAGS = [
    # Ensure TCMalloc itself builds without errors, even if its dependencies
    # aren't necessarily -Werror clean.
    "-Werror",
    "-Wno-deprecated-declarations",
    "-Wno-deprecated-volatile",
    "-Wno-implicit-int-float-conversion",
    "-Wno-sign-compare",
    "-Wno-uninitialized",
    "-Wno-unused-function",
    "-Wno-unused-variable",
]

TCMALLOC_GCC_FLAGS = [
    # Ensure TCMalloc itself builds without errors, even if its dependencies
    # aren't necessarily -Werror clean.
    "-Werror",
    "-Wno-array-bounds",
    "-Wno-attribute-alias",
    "-Wno-deprecated-declarations",
    "-Wno-sign-compare",
    "-Wno-stringop-overflow",
    "-Wno-uninitialized",
    "-Wno-unused-function",
    # https://gcc.gnu.org/bugzilla/show_bug.cgi?id=66425
    "-Wno-unused-result",
    "-Wno-unused-variable",
]

TCMALLOC_DEFAULT_COPTS = select({
    "//tcmalloc:llvm": TCMALLOC_LLVM_FLAGS,
    "//conditions:default": TCMALLOC_GCC_FLAGS,
})
