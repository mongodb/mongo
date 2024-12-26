#!/usr/bin/env python3

""" Source scan script.

This script scans WiredTiger.

"""

import sys, os

# layercparse is a library written and maintained by the WiredTiger team.
import layercparse as lcp
from layercparse.scan_sources_tool import scan_sources_main

WT_DEFS_RELATIVE_PATH = "dist/modularity/wt_defs.py"

def main():
    lcp.Log.module_name_mismatch.enabled = False

    return scan_sources_main(WT_DEFS_RELATIVE_PATH)

if __name__ == "__main__":
    try:
        sys.exit(main())
    except (KeyboardInterrupt, BrokenPipeError):
        print("\nInterrupted")
        sys.exit(1)
    except OSError as e:
        print(f"\n{e.strerror}: {e.filename}")
        sys.exit(1)

