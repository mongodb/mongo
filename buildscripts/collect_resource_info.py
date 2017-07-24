#!/usr/bin/env python

"""
Collect system resource information on processes running in Evergreen on a given interval.
"""

from __future__ import absolute_import
from __future__ import print_function

import contextlib
from datetime import datetime
import json
import optparse
import os
import sys
import time

from bson.json_util import dumps
import requests

@contextlib.contextmanager
def open_or_use_stdout(filename):
    """
    Opens the specified file for writing, or returns sys.stdout if filename is "-".
    """

    if filename == "-":
        yield sys.stdout
        return

    line_buffered = 1
    fp = open(filename, "w", line_buffered)
    try:
        yield fp
    finally:
        fp.close()

def main():

    usage = "usage: %prog [options] interval"
    parser = optparse.OptionParser(description=__doc__, usage=usage)
    parser.add_option("-i", "--interval",
                      dest="interval",
                      default=5,
                      type="int",
                      help="Collect system resource information every <interval> seconds. "
                           "Default is every 5 seconds.")
    parser.add_option("-o", "--output-file",
                      dest="outfile",
                      default="-",
                      help="If '-', then the file is written to stdout."
                           " Any other value is treated as the output file name. By default,"
                           " output is written to stdout.")

    (options, args) = parser.parse_args()

    with open_or_use_stdout(options.outfile) as fp:
        while True:
            # Requires the Evergreen agent to be running on port 2285.
            response = requests.get("http://localhost:2285/status")
            if response.status_code != requests.codes.ok:
                print("Received a {} HTTP response: {}".format(response.status_code, response.text),
                      file=sys.stderr)
                time.sleep(options.interval)
                continue

            timestamp = datetime.now()
            try:
                res_json = response.json()
            except ValueError:
                print("Invalid JSON object returned with response: {}".format(response.text),
                      file=sys.stderr)
                time.sleep(options.interval)
                continue

            sys_res_dict = {}
            sys_res_dict["timestamp"] = timestamp
            sys_info = res_json["sys_info"]
            sys_res_dict["num_cpus"] = sys_info["num_cpus"]
            sys_res_dict["mem_total"] = sys_info["vmstat"]["total"]
            sys_res_dict["mem_avail"] = sys_info["vmstat"]["available"]
            ps_info = res_json["ps_info"]
            for process in ps_info:
                try:
                    sys_res_dict["pid"] = process["pid"]
                    sys_res_dict["ppid"] = process["parentPid"]
                    sys_res_dict["num_threads"] = process["numThreads"]
                    sys_res_dict["command"] = process.get("command", "")
                    sys_res_dict["cpu_user"] = process["cpu"]["user"]
                    sys_res_dict["cpu_sys"] = process["cpu"]["system"]
                    sys_res_dict["io_write"] = process["io"]["writeBytes"]
                    sys_res_dict["io_read"] = process["io"]["readBytes"]
                    sys_res_dict["mem_used"] = process["mem"]["rss"]
                except KeyError:
                    # KeyError may occur as a result of file missing from /proc, likely due to
                    # process exiting.
                    continue

                print(dumps(sys_res_dict, sort_keys=True), file=fp)

                # Flush internal buffers associated with file to disk.
                os.fsync(fp.fileno())
            time.sleep(options.interval)

if __name__ == "__main__":
    main()
