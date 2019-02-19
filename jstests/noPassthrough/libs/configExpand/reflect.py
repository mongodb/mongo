#! /usr/bin/env python3
"""Simple reflection script.
   Sends argument back as provided.
   Optionally sleeps for `--sleep` seconds."""

import argparse
import sys
import time


def main():
    """Main Method."""

    parser = argparse.ArgumentParser(description='MongoDB Mock Config Expandsion EXEC Endpoint.')
    parser.add_argument('-s', '--sleep', type=int, default=0,
                        help="Add artificial delay for timeout testing")
    parser.add_argument('value', type=str, help="Content to reflect to stdout")

    args = parser.parse_args()

    if args.sleep > 0:
        try:
            time.sleep(args.sleep)
        except BrokenPipeError:
            # Let our caller kill us while we sleep
            pass

    sys.stdout.write(args.value)


if __name__ == '__main__':
    main()
