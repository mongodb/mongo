#!/bin/bash
#
#  Copyright (c) 2022 Alexander Grund
#
#  Distributed under the Boost Software License, Version 1.0.
#  https://www.boost.org/LICENSE_1_0.txt

# Format source code with clang-format

set -euo pipefail

CLANG_FORMAT="clang-format-14"

if ! command -v "$CLANG_FORMAT" &> /dev/null; then
    echo "You need $CLANG_FORMAT in your PATH"
    exit 1
fi

cd "$(dirname "$0")/.."

find . -type f \( -name '*.cpp' -o -name '*.hpp' \) -exec "$CLANG_FORMAT" -i --style=file {} \;
