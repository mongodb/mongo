#!/bin/sh

set -e

GOLDEN_COMP_DIR="$ZSTD_REPO_DIR/tests/golden-compression/"
GOLDEN_DICT_DIR="$ZSTD_REPO_DIR/tests/golden-dictionaries/"

zstd -D "$GOLDEN_DICT_DIR/http-dict-missing-symbols" "$GOLDEN_COMP_DIR/http" -o http.zst
zstd -D "$GOLDEN_DICT_DIR/http-dict-missing-symbols" -t http.zst
