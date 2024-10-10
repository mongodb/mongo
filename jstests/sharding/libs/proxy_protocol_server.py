#! /usr/bin/env python3
"""
Python script to interact with proxy protocol server.
"""

import sys

from proxyprotocol.server.main import *

if __name__ == "__main__":
    print("Starting proxy protocol server...")
    sys.exit(main())
