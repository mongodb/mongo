#!/usr/bin/env bash
# shellcheck disable=SC2086,SC2129,SC2034

set -euxo pipefail
# execute this script in a separate, early step

LOCAL_BUILDS="${GITHUB_WORKSPACE}/builds"

# To install and cache our dependencies we need an absolute path
# that does not change, is writable, and resides within
# GITHUB_WORKSPACE.
# On macOS GITHUB_WORKSPACE includes the github runner version,
# so it does not remain constant.
# This causes problems with, for example, pkgconfig files
# referencing paths that no longer exist.
CACHE_DIR="installs"
mkdir -p "${CACHE_DIR}"

if [[ "${RUNNER_OS}" = "Windows" ]]
then
  rnp_local_installs="${RUNNER_TEMP}/rnp-local-installs"
else
  rnp_local_installs=/tmp/rnp-local-installs
fi

ln -s "$GITHUB_WORKSPACE/installs" "${rnp_local_installs}"
LOCAL_INSTALLS="${rnp_local_installs}"

# When building packages, dependencies with non-standard installation paths must
# be found by the (DEB) package builder.
BOTAN_INSTALL="${rnp_local_installs}/botan-install"
JSONC_INSTALL="${rnp_local_installs}/jsonc-install"
GPG_INSTALL="${rnp_local_installs}/gpg-install"

# set this explicitly since we don't want to cache the rnp installation
RNP_INSTALL="${GITHUB_WORKSPACE}/rnp-install"

for var in \
  LOCAL_BUILDS \
  CACHE_DIR \
  LOCAL_INSTALLS \
  BOTAN_INSTALL \
  JSONC_INSTALL \
  GPG_INSTALL \
  RNP_INSTALL
do
  val="${!var}"

  # Replace all backslashes with forward slashes, for cmake, so the following
  # error would not come up:
  #
  # Invalid character escape '\a'.
  #
  if [[ "${RUNNER_OS}" = "Windows" ]]
  then
    val="${val//\\/\/}"
  fi

  echo "${var}=${val}" >> "$GITHUB_ENV"
done
