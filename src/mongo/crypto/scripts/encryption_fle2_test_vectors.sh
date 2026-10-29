#!/bin/bash

set -e

data="$(xxd -c10000 -p)"
key="$(xxd -c100 -p -l32 /dev/urandom)"
iv="$(xxd -c100 -p -l16 /dev/urandom)"
ciphertext="$(echo "$data" | xxd -r -p | openssl enc -aes-256-ctr -K "$key" -iv "$iv" | xxd -c10000 -p)"

cat <<EOF
    // clang-format off
    vector.d = "$data";
    vector.k = "$key";
    vector.iv = "$iv";
    vector.c = "$ciphertext";
    vector.r = "$iv$ciphertext";
    // clang-format on
EOF
