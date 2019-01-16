#!/bin/bash

python2 buildscripts/scons.py mongod mongo mongos --disable-warnings-as-errors -j8  --release --distance-expression-use-avx2
