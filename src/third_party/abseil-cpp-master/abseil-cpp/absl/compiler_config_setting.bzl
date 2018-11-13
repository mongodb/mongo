#
# Copyright 2018 The Abseil Authors.
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
#

"""Creates config_setting that allows selecting based on 'compiler' value."""

def create_llvm_config(name, visibility):
  # The "do_not_use_tools_cpp_compiler_present" attribute exists to
  # distinguish between older versions of Bazel that do not support
  # "@bazel_tools//tools/cpp:compiler" flag_value, and newer ones that do.
  # In the future, the only way to select on the compiler will be through
  # flag_values{"@bazel_tools//tools/cpp:compiler"} and the else branch can
  # be removed.
  if hasattr(cc_common, "do_not_use_tools_cpp_compiler_present"):
    native.config_setting(
      name = name,
      flag_values = {
          "@bazel_tools//tools/cpp:compiler": "llvm",
      },
      visibility = visibility,
    )
  else:
    native.config_setting(
        name = name,
        values = {"compiler": "llvm"},
        visibility = visibility,
    )
