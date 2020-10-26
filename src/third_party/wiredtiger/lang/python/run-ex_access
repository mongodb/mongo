#!/bin/sh

PYTHON=${PYTHON:-python3}
rm -rf WT_TEST ; mkdir WT_TEST

exec env LD_LIBRARY_PATH=../../.libs DYLD_LIBRARY_PATH=../../.libs PYTHONPATH=.:${srcdir} ${PYTHON} -S ${srcdir}/../../examples/python/ex_access.py 
