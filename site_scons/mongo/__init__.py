# -*- mode: python; -*-

# General utility functions live in this file.

def print_build_failures():
    from SCons.Script import GetBuildFailures
    for bf in GetBuildFailures():
        print "%s failed: %s" % (bf.node, bf.errstr)
