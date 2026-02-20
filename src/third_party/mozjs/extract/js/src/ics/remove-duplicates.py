#!/usr/bin/env python3

# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

import glob
import hashlib
import os

have_hashes = set()

for file in glob.glob("IC-*"):
    with open(file, "rb") as f:
        content = f.read()
        h = hashlib.new("sha256")
        h.update(content)
        digest = h.hexdigest()
        if digest in have_hashes:
            print("Removing: %s" % file)
            os.unlink(file)
        have_hashes.add(digest)
