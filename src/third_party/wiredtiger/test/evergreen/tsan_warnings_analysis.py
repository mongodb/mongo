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
import argparse, os, subprocess
import re
# The script will look through all tsan logs from the current directory
# and collect all the warnings the found under tsan logs.
#
# The TSAN configuration will need option log_path to create the tsan logs.
#
# FIXME-WT-15235: The current filtering of TSAN warnings, can hide true
# positive warnings if they have the same summary but a different TSAN
# trace.
def get_line_last_modified_times(file_path, line_number):
    """
    Get the last modification time (UNIX timestamp) for the provided line and file using git blame.

    :param line_number: Line number to analyze.
    :return: Last modified timestamp.
    """
    # Run `git blame` to retrieve metadata for found line number
    git_command = ["git", "blame", "--line-porcelain", "HEAD", f"-L {line_number},{line_number}", "--", file_path]
    result = subprocess.run(git_command, capture_output=True, text=True, check=True)

    timestamp = None
    output_lines = result.stdout.splitlines()
    for line in output_lines:
        # Extract the UNIX timestamp
        if line.startswith("author-time"):
            timestamp = int(line.split()[1])
    return timestamp

def get_tsan_warnings():
    """
    Get the TSAN warnings from the log files.
    :return: Set of unique TSAN warnings.
    """
    tsan_warnings_set = set()
    current_dir = os.getcwd()

    # Loop through WT root directory and search for tsan logs.
    for root, _, files in os.walk(current_dir):
        for file_name in files:
            # Check if the file starts with "tsan"
            if file_name.startswith("tsan_logs"):
                file_path = os.path.join(root, file_name)  # Get the full path to the file
                with open(file_path, "r") as file:
                    for line in file:
                        if (not line.startswith("SUMMARY:")):
                            continue
                        # Strip away the unnecessary information
                        pattern_to_remove = r"/.*/wiredtiger/"
                        cleaned_text = re.sub(pattern_to_remove, "", line).strip()

                        # Strip away the column line information.
                        pattern_to_remove = r':(\d+):\d+'
                        cleaned_text = re.sub(pattern_to_remove, r':\1', cleaned_text).strip()

                        tsan_warnings_set.add(cleaned_text)
    return tsan_warnings_set


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('-t', '--timestamp', help='Filter all warnings that happened before the timestamp')

    args = parser.parse_args()

    tsan_warnings_set = get_tsan_warnings()
    filter_tsan_warnings = set()

    if (args.timestamp):
        for tsan_warning in tsan_warnings_set:
            pattern_to_capture = r"data race (/wiredtiger/.*):(\d+)"
            capture = re.search(pattern_to_capture, tsan_warning)
            if (capture):
                timestamp = get_line_last_modified_times(capture.group(1), capture.group(2))
                timestamp_filter = int(args.timestamp)
                if (timestamp_filter and timestamp_filter <= timestamp):
                    filter_tsan_warnings.add(tsan_warning)
        tsan_warnings_set = filter_tsan_warnings

    if len(tsan_warnings_set) == 0:
        print("No TSAN warnings to fix!")
    else:
        print("Total warnings:")
        print("\n".join(tsan_warnings_set))
        print(f"Overall TSAN Warnings: {len(tsan_warnings_set)}")

if __name__ == '__main__':
    main()
