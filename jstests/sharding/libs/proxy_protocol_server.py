#! /usr/bin/env python3
"""
Python script to interact with proxy protocol server.
"""

from proxyprotocol.server.main import *
import sys

if __name__ == '__main__':
    print("Starting proxy protocol server...")
    sys.exit(main())
