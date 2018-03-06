#!/usr/bin/env python2
"""
Computes a SHA256 sum of a file.
Accepts a file path, prints the hex encoded hash to stdout.
"""

import sys
from hashlib import sha256

with open(sys.argv[1], 'rb') as f:
    print(sha256(f.read()).hexdigest())
