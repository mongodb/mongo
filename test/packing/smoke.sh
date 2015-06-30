#! /bin/sh

# Smoke-test integer packing as part of running "make check".  Don't just run
# everything because some of the code in this directory is aimed at testing the
# performance of the packing code

./packing-test

./intpack-test3
