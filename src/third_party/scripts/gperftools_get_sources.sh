#!/bin/bash
LIBRARY_DIR="$(git rev-parse --show-toplevel)/src/third_party/gperftools-2.7"
exec "$LIBRARY_DIR/scripts/import.sh" "$@"
