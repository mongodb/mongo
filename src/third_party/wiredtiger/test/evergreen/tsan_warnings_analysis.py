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

    # Check that requested line number and file has a timestamp.
    if (timestamp is None):
        print("Error: Requested line and file path doesn't have a timestamp")
        exit(1)

    return timestamp

def get_tsan_warnings():
    """
    Get the TSAN warnings from the log files.
    :return: Set of unique TSAN warnings.
    """
    tsan_warnings_dict = dict()
    current_dir = os.getcwd()

    # Loop through WT root directory and search for tsan logs.
    for root, _, files in os.walk(current_dir):
        for file_name in files:
            # Check if the file starts with "tsan"
            if file_name.startswith("tsan_logs"):
                file_path = os.path.join(root, file_name)  # Get the full path to the file
                with open(file_path, "r") as file:
                    start_record = False
                    warning_lines = []
                    for line in file:
                        if ("WARNING:" in line.strip()):
                            start_record = True
                            continue
                        if start_record:
                            warning_lines.append(line.strip())
                            if (line.startswith("SUMMARY:")):
                                # Strip away the path
                                pattern_to_remove = r"/.*/wiredtiger/"
                                cleaned_text = re.sub(pattern_to_remove, "", line).strip()

                                # Strip away the column line information.
                                pattern_to_remove = r':(\d+):\d+'
                                cleaned_text = re.sub(pattern_to_remove, r':\1', cleaned_text).strip()
                                tsan_warnings_dict[cleaned_text] = (file_name, warning_lines.copy())

                                # Restart the warning recording.
                                warning_lines = []
                                start_record = False
    return tsan_warnings_dict


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('-t', '--timestamp', help='Filter all warnings that happened before the timestamp')

    args = parser.parse_args()

    tsan_warnings_dict = get_tsan_warnings()
    filter_tsan_warnings = dict()

    if (args.timestamp):
        for tsan_warning, tsan_tuple in tsan_warnings_dict.items():
            pattern_to_capture = r"data race (.*):(\d+)"
            capture = re.search(pattern_to_capture, tsan_warning)
            # It is possible for TSAN warnings to not have line number information.
            # In such case, add the TSAN warning to the filtered tsan warnings.
            if (not capture):
                print(f"Error: Unable to parse the tsan warning: {tsan_warning}")
                filter_tsan_warnings[tsan_warning] = tsan_tuple
                continue
            file_path = capture.group(1)
            line_number = int(capture.group(2))
            timestamp = get_line_last_modified_times(file_path, line_number)
            timestamp_filter = int(args.timestamp)
            if (timestamp_filter <= timestamp):
                filter_tsan_warnings[tsan_warning] = tsan_tuple
        tsan_warnings_dict = filter_tsan_warnings

    if len(tsan_warnings_dict) == 0:
        print("No TSAN warnings to fix!")
    else:
        print("Total warnings:")
        for _, (tsan_log, warning_lines) in tsan_warnings_dict.items():
            print("=" * 150)
            print("TSAN log: " + tsan_log)
            print("\n".join(warning_lines))
            print("=" * 150)
        print(f"Overall TSAN Warnings: {len(tsan_warnings_dict)}")

if __name__ == '__main__':
    main()
