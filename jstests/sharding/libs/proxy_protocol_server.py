#! /usr/bin/env python3
"""
Python script to interact with proxy protocol server.

This script is a wrapper around the [proxy-protocol][1] package.
The most recent source code is available on [GitHub][2].

The installed version is listed in the
"[tool.poetry.group.testing.dependencies]" section of `pyproject.toml`.

[1]: https://pypi.org/project/proxy-protocol/
[2]: https://github.com/icgood/proxy-protocol/
"""

import sys

from proxyprotocol.server.main import *

if __name__ == "__main__":
    print("Starting proxy protocol server...")
    sys.exit(main())
