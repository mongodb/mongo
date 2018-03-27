#!/usr/bin/env python2
"""Prune check program.

This program stamps the shared scons directory with a timestamp so we can
determine the last prune time and run the prune script on a schedule.
It is meant to be invoked from the shell:

if python prune_check.py; then
  echo 'Pruning'
else
  echo 'Less than 24 hours, waiting ...'
fi

The script can be invoked with optional arguments for mount point and 'seconds
since last prune' (default is 86400 - 24 hours). Use -h to see options and defaults.

python prune_check.py -m '/mount_point' -p 86400

To write the latest timestamp to a directory

python prune_check.py -w

If it is time to prune (ie. more than 24 hours since the last timestamp),
the script exits with a 0 return code.
Otherwise the script returns exit code 1.
"""

import argparse
from datetime import datetime
import os
import sys
import time

DATE_TIME_STR = "%Y-%m-%d %H:%M:%S"


def get_prune_file_path(mount_point):
    """Get the shared scons directory for this AMI."""
    with open('/etc/mongodb-build-system-id', 'r') as fh:
        uuid = fh.read().strip()
    return os.path.join(mount_point, uuid, 'info', 'last_prune_time')


def write_last_prune_time(last_prune_time, prune_file_path):
    """Write the last prune timestamp in a 'last_prune_time' file."""
    with open(prune_file_path, 'w') as fh:
        fh.write(last_prune_time.strftime(DATE_TIME_STR) + '\n')


def retrieve_last_prune_time(prune_file_path):
    """Get the last prune time from the 'last_prune_time' file."""
    if os.path.isfile(prune_file_path):
        with open(prune_file_path, 'r') as fh:
            last_prune_time_str = fh.read().strip()
            last_prune_time = datetime.strptime(last_prune_time_str, DATE_TIME_STR)
    else:
        last_prune_time = datetime.utcnow()
        write_last_prune_time(last_prune_time, prune_file_path)

    return last_prune_time


def check_last_prune_time(args):
    """Return exit code 0 if time to run again, else return exit code 1.

    This is meant to be called from the shell
    """

    seconds_since_last_prune = args.prune_seconds
    prune_file_path = get_prune_file_path(args.mount_point)

    now = datetime.utcnow()
    last_prune_time = retrieve_last_prune_time(prune_file_path)

    diff = now - last_prune_time

    # if it's been longer than 'seconds_since_last_prune', return 0 to the shell.
    # A 0 return code signals our Evergreen task that we should run the prune script.
    # Otherwise, return 1 and skip pruning.
    if diff.total_seconds() > seconds_since_last_prune:
        print("It has been {0:.2f} seconds ({1:.2f} hours) since last prune.".format(
            diff.total_seconds(),
            diff.total_seconds() / 60 / 60))
        sys.exit(0)
    else:
        print("It has been {0:.2f} seconds ({1:.2f} hours) since last prune.".format(
            diff.total_seconds(),
            diff.total_seconds() / 60 / 60))
        sys.exit(1)


def get_command_line_args():
    """Get the command line arguments."""
    parser = argparse.ArgumentParser()
    parser.add_argument('-m', '--mount_point', type=str, required=False,
                        help="The base mount where efs is mounted. Default is '/efs'",
                        default='/efs')
    parser.add_argument('-p', '--prune_seconds', type=int, required=False,
                        help="Seconds to wait since last prune - default is 86400 (one day)",
                        default=86400)
    parser.add_argument('-w', '--write_prune_time', action='store_true', required=False,
                        help="Write the latest prune time")
    args = parser.parse_args()
    return args


def main():
    """Execute Main program."""
    args = get_command_line_args()
    mount_point = args.mount_point

    if args.write_prune_time:
        write_last_prune_time(datetime.utcnow(), get_prune_file_path(mount_point))
    else:
        check_last_prune_time(args)


if __name__ == '__main__':
    main()
