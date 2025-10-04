#!/usr/bin/env python3
# ################################################################
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under both the BSD-style license (found in the
# LICENSE file in the root directory of this source tree) and the GPLv2 (found
# in the COPYING file in the root directory of this source tree).
# You may select, at your option, one of the above-listed licenses.
# ################################################################

import os
import subprocess
import sys

if len(sys.argv) != 3:
	print(f"Usage: {sys.argv[0]} FILE SIZE_LIMIT")
	sys.exit(1)

file = sys.argv[1]
limit = int(sys.argv[2])

if not os.path.exists(file):
	print(f"{file} does not exist")
	sys.exit(1)

size = os.path.getsize(file)

if size > limit:
	print(f"file {file} is {size} bytes, which is greater than the limit of {limit} bytes")
	sys.exit(1)
