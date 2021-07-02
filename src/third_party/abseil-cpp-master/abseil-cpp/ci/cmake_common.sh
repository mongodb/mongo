# Copyright 2020 The Abseil Authors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# The commit of GoogleTest to be used in the CMake tests in this directory.
# Keep this in sync with the commit in the WORKSPACE file.
readonly ABSL_GOOGLETEST_COMMIT="8567b09290fe402cf01923e2131c5635b8ed851b"

# Avoid depending on GitHub by looking for a cached copy of the commit first.
if [[ -r "${KOKORO_GFILE_DIR:-}/distdir/${ABSL_GOOGLETEST_COMMIT}.zip" ]]; then
  DOCKER_EXTRA_ARGS="--mount type=bind,source=${KOKORO_GFILE_DIR}/distdir,target=/distdir,readonly ${DOCKER_EXTRA_ARGS:-}"
  ABSL_GOOGLETEST_DOWNLOAD_URL="file:///distdir/${ABSL_GOOGLETEST_COMMIT}.zip"
else
  ABSL_GOOGLETEST_DOWNLOAD_URL="https://github.com/google/googletest/archive/${ABSL_GOOGLETEST_COMMIT}.zip"
fi
