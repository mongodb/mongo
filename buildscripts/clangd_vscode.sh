#!/usr/bin/env bash

# Collect all passed arguments
ARGS=("$@")

# Ordered list of possible clangd locations
CANDIDATES=(
    "$(command -v custom-clangd)"
    "$(find .compiledb/compiledb-*/external/mongo_toolchain_v5/v5/bin/clangd)"
    "$(find bazel-*/external/mongo_toolchain_v5/v5/bin/clangd)"
    "/opt/mongodbtoolchain/v5/bin/clangd"
)

# Find the first available clangd
for CANDIDATE in "${CANDIDATES[@]}"; do
    if [[ -x "$CANDIDATE" ]]; then
        CLANGD="$CANDIDATE"
        echo "[INFO] Using clangd at: $CLANGD" >&2
        break
    fi
done

# Fail if no clangd was found
if [[ -z "$CLANGD" ]]; then
    echo "[ERROR] clangd not found in any of the expected locations." >&2
    exit 1
fi

FINAL_ARGS=(
    "${ARGS[@]}"
    "--query-driver=./**/*{clang,gcc,g++}*" # allow any clang or gcc binary in the repo
    "--header-insertion=never"
)

# Log the full command (optional)
echo "[INFO] Executing: $CLANGD ${FINAL_ARGS[*]}" >&2

# Run clangd with the final arguments
"$CLANGD" "${FINAL_ARGS[@]}"
