#!/bin/bash

set -e

data="$(xxd -c10000 -p)"
key="$(xxd -c100 -p -l32 /dev/urandom)"
iv="$(xxd -c100 -p -l16 /dev/urandom)"
ciphertext="$(echo "$data" | xxd -r -p | openssl enc -aes-256-ctr -K "$key" -iv "$iv" | xxd -c10000 -p)"

cat <<EOF
    // clang-format off
    vector.d = "$data"_sd;
    vector.k = "$key"_sd;
    vector.iv = "$iv"_sd;
    vector.c = "$ciphertext"_sd;
    vector.r = "$iv$ciphertext"_sd;
    // clang-format on
EOF
