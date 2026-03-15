#!/usr/bin/env python3
#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.

import argparse
import os
import re
import shutil

def collect_stat_files(destination_dir, source_dir, regex):
    """
    Collect all stat files from a source directory and copy them into a specified destination directory.

    :param destination_dir: The destination directory where stat files are copied to.
    :param source_dir: The source directory to search for stat files.
    :param regex: A compiled regular expression to match stat file names.
    """

    # Remove the leading "./" from the source directory name and replace all "/" with "-".
    # This creates a unique directory name for the stat files being collected from this location.
    destination_sub_dir = os.path.join(destination_dir, source_dir[2:].replace("/", "-"))

    # Create the subdirectory that the files from this location will be copied to.
    if not os.path.exists(destination_sub_dir):
        os.makedirs(destination_sub_dir)

    for item in os.listdir(source_dir):
        path = os.path.join(source_dir, item)
        if os.path.isfile(path) and regex.match(item):
            file_path = os.path.join(source_dir, item)
            shutil.copy(file_path, destination_sub_dir)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('-d', '--destination-dir', required=True, help='Directory to collect stat files into for packing.')
    args = parser.parse_args()

    destination_dir = args.destination_dir

    regex = re.compile(r'WiredTigerStat.*')
    for walk_dir, _, files in os.walk("."):
        if destination_dir in walk_dir:
            # Skip searching for stat files in the destination directory, since that's where we're collecting them and we don't want to copy files that we've already collected.
            continue
        for file in files:
            if regex.match(file):
                # If current directory contains any stat files, collect them all into a single location for packing.
                collect_stat_files(destination_dir, walk_dir, regex)

                # Finished searching in this directory, move on to the next one.
                break

if __name__ == "__main__":
    main()
