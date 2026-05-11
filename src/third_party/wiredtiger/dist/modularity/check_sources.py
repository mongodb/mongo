#!/usr/bin/env python3
#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.

""" Access checker script.

This script checks that WiredTiger sources comply with modularity rules
described in MODULARITY.md.

"""

import sys, os

# layercparse is a library written and maintained by the WiredTiger team.
import layercparse as lcp

WT_DEFS_RELATIVE_PATH = "dist/modularity/wt_defs.py"

def main():
    lcp.Log.module_name_mismatch.enabled = False

    rootPath = os.path.realpath(sys.argv[1])
    lcp.setRootPath(rootPath)
    wt_defs = lcp.load_code_config(rootPath, WT_DEFS_RELATIVE_PATH)
    lcp.setModules(wt_defs["modules"])

    files = lcp.get_files()  # list of all source files
    for file in wt_defs["extraFiles"]:
        files.insert(0, os.path.join(os.path.realpath(rootPath), file))

    _globals = lcp.Codebase()
    for macro in wt_defs["extraMacros"]:
        _globals.addMacro(**macro)

    _globals.scanFiles(files)

    lcp.AccessCheck(_globals).checkAccess()

    return not lcp.workspace.errors

if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nInterrupted")
        sys.exit(1)
    except OSError as e:
        print(f"\n{e.strerror}: {e.filename}")
        sys.exit(1)

