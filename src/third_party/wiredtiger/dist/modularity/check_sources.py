#!/usr/bin/env python3

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

