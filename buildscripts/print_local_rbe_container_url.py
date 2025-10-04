#!/usr/bin/env python3

import os
import sys

if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from buildscripts.local_rbe_container_url import calculate_local_rbe_container_url


def main():
    os.chdir(os.environ.get("BUILD_WORKSPACE_DIRECTORY", "."))
    print(calculate_local_rbe_container_url())


if __name__ == "__main__":
    exit(main())
