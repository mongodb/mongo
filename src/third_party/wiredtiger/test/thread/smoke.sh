#! /bin/sh

# Smoke-test format as part of running "make check".

./t -t f || exit 1
./t -S -F -t f || exit 1

./t -t r || exit 1
./t -S -F -t r || exit 1

./t -t v || exit 1
./t -S -F -t v || exit 1
