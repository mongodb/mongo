#!/bin/bash

set -e

tmp=/tmp/aead-tests
mkdir -p $tmp
cd $tmp

dd status=none of=$tmp/M.bin
dd status=none if=/dev/urandom of=AD.bin bs=${ADLENGTH:-18} count=1
dd status=none if=/dev/urandom of=IV.bin bs=16 count=1
dd status=none if=/dev/urandom of=Ke.bin bs=32 count=1
dd status=none if=/dev/urandom of=Km.bin bs=32 count=1

openssl enc -aes-256-${MODE:-ctr} -in M.bin -K $(xxd -c 100 -p Ke.bin) -iv $(xxd -c 100 -p IV.bin) -out S.bin

# digest tested per: https://datatracker.ietf.org/doc/html/rfc4231#section-4.3
# echo -n "what do ya want for nothing?" | openssl dgst -sha256 -mac hmac -macopt hexkey:4a656665 -hex
cat AD.bin IV.bin S.bin | openssl dgst -sha256 -mac hmac -macopt hexkey:$(xxd -c 100 -p Km.bin) -binary -out T.bin

cat IV.bin S.bin T.bin >C.bin

cat <<EOF
    // clang-format off
    vector.ad = "$(xxd -c 10000 -p AD.bin)"_sd;
    vector.c = "$(xxd -c 10000 -p C.bin)"_sd;
    vector.iv = "$(xxd -c 10000 -p IV.bin)"_sd;
    vector.ke = "$(xxd -c 10000 -p Ke.bin)"_sd;
    vector.km = "$(xxd -c 10000 -p Km.bin)"_sd;
    vector.m = "$(xxd -c 10000 -p M.bin)"_sd;
    vector.s = "$(xxd -c 10000 -p S.bin)"_sd;
    vector.t = "$(xxd -c 10000 -p T.bin)"_sd;
    // clang-format on
EOF
