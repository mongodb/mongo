#!/bin/sh

set -e

GOLDEN_DIR="$ZSTD_REPO_DIR/tests/golden-compression/"
cp -r "$GOLDEN_DIR" golden/

zstd -rf golden/ --output-dir-mirror golden-compressed/
zstd -r -t golden-compressed/

zstd --target-compressed-block-size=1024 -rf golden/ --output-dir-mirror golden-compressed/
zstd -r -t golden-compressed/

# PR #3517 block splitter corruption test
zstd -rf -19 --zstd=mml=7 golden/ --output-dir-mirror golden-compressed/
zstd -r -t golden-compressed/