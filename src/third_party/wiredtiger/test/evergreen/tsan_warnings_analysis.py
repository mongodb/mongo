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
import os, sys
import re, subprocess
# The script intends to generate warnings from WiredTiger's examples, csuite, python and test/format testing.
#
# Running the whole suite causes enormous number of TSAN warnings. The amount of I/O causes slowness in the
# system leading to non-deterministic results. To ensure deterministic results run with only examples suite.

# Configure log path in TSAN options.
current_dir = os.getcwd()
existing_tsan_options = os.environ.get("TSAN_OPTIONS")
os.environ["TSAN_OPTIONS"] = f"{existing_tsan_options}:log_path={current_dir}/tsan_logs:exitcode=0"

# Start with examples suite.
test_tasks = [
    "ctest -j 8 --test-dir examples/c"
]

# Run the tasks in list.
print("Tasks to be executed")
print(test_tasks)
for task in test_tasks:
    try:
        # Tasks are allowed to run with maximum of three hours.
        subprocess.run(task.split(), check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=60 * 60 * 3)
    except subprocess.CalledProcessError as exception:
        print(f'Command {exception.cmd} failed with error {exception.returncode}')
    except subprocess.TimeoutExpired as exception:
        print(f'Command {exception.cmd} timed out')
        sys.exit(1)

# Loop through WT root directory and search for tsan logs.
tsan_warnings_set = set()
for tsan_log in os.listdir(current_dir):
    # Check if the file starts with "tsan"
    if tsan_log.startswith("tsan"):
        with open(tsan_log, "r") as file:
            for line in file:
                if (not line.startswith("SUMMARY:")):
                    continue
                # Strip away the unnecessary information
                pattern_to_remove = r"/data/mci/.*/wiredtiger/"
                cleaned_text = re.sub(pattern_to_remove, "", line).strip()

                tsan_warnings_set.add(cleaned_text)

print("Unique warnings:")
print("\n".join(tsan_warnings_set))
print(f"Overall TSAN Warnings: {len(tsan_warnings_set)}")
