# -*- mode: python; -*-

# General utility functions live in this file.

import bisect

def print_build_failures():
    from SCons.Script import GetBuildFailures
    for bf in GetBuildFailures():
        print("%s failed: %s" % (bf.node, bf.errstr))

def insort_wrapper(target_list, target_string):
    """
    Removes instances of empty list inside the list before handing it to insort.
    """
    from SCons.Util import flatten
    target_list[:] = flatten(target_list)
    bisect.insort(target_list, target_string)
