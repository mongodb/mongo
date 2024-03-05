#!/bin/sh
# This script clones Metrix++ and checks out a specific commit

# Metrix++ is made available as source code in GitHub.
git clone https://github.com/metrixplusplus/metrixplusplus metrixplusplus || exit

cd metrixplusplus || exit

# Metrix++ has has a release archive here: https://github.com/metrixplusplus/metrixplusplus/releases
# But the latest code (which we use) is not available in that release archive. This means that a git hash is the
# best way to access the latest version in a controlled way, rather than have updates potentially occur unexpectedly,
# Check out the version from 18th Dec 2022, which is the latest commit as of 28th Feb 2024.
git checkout 78dc5380de9aaa3d615f8be6c84e90cb2ae0d90b || exit

cd ..
