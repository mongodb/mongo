#!/usr/bin/env python3

import argparse

from checker import Checker
from operation import Operation

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Verify actions taken by rollback to stable from verbose messages.')
    parser.add_argument('file', type=str, help='the log file to parse verbose messages from')
    args = parser.parse_args()

    checker = Checker()
    with open(args.file) as f:
        for line in f:
            if 'WT_VERB_RTS' in line:
                op = Operation(line)
                checker.apply(op)
