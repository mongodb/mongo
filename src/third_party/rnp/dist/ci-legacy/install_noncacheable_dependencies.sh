#!/usr/bin/env bash
# shellcheck disable=SC1091
set -exu

. ci/env.inc.sh

"${OS}_install"
install_static_noncacheable_build_dependencies_if_needed "$@"
