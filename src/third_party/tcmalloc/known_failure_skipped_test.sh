#!/usr/bin/env bash

set -euo pipefail

known_failure="${1:-unknown}"

cat <<EOF
Known failing upstream tcmalloc test intentionally skipped from mongo_tcmalloc_unittest:
  ${known_failure}

The real test target is tagged mongo_tcmalloc_known_failure.
EOF
