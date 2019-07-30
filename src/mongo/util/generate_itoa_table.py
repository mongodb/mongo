#!/usr/bin/env python3
#
#     Copyright (C) 2019-present MongoDB, Inc.
#
#     This program is free software: you can redistribute it and/or modify
#     it under the terms of the Server Side Public License, version 1,
#     as published by MongoDB, Inc.
#
#     This program is distributed in the hope that it will be useful,
#     but WITHOUT ANY WARRANTY; without even the implied warranty of
#     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#     Server Side Public License for more details.
#
#     You should have received a copy of the Server Side Public License
#     along with this program. If not, see
#     <http://www.mongodb.com/licensing/server-side-public-license>.
#
#     As a special exception, the copyright holders give permission to link the
#     code of portions of this program with the OpenSSL library under certain
#     conditions as described in each individual source file and distribute
#     linked combinations including the program with the OpenSSL library. You
#     must comply with the Server Side Public License in all respects for
#     all of the code used other than as permitted herein. If you modify file(s)
#     with this exception, you may extend this exception to your version of the
#     file(s), but you are not obligated to do so. If you do not wish to do so,
#     delete this exception statement from your version. If you delete this
#     exception statement from all source files in the program, then also delete
#     it in the license file.

# Generate a table for itoa.cpp
import argparse
import io
import math
import sys

def main():
    """Execute Main Entry point."""
    parser = argparse.ArgumentParser(description='MongoDB Itoa Table Generator.')

    parser.add_argument('--digit_count', type=int, required=True, help="Number of Digits to Generate")

    parser.add_argument('--output', type=str, required=True, help="Output file")

    args = parser.parse_args()

    digits = args.digit_count

    with io.open(args.output, mode='w') as file_handle:

        file_handle.write(f'#define ITOA_TABLE_EXPAND_{digits}(makeEntry_, comma_) \\\n')
        sep = ''
        for i in range(pow(10, digits)):
            n = len(f'{i}')
            s = f'{i:0{digits}d}'
            cs = ','.join([f"'{c}'" for c in s])
            file_handle.write(f'{sep}    makeEntry_({n}, ({cs:s}))')
            sep = " comma_ \\\n"

        file_handle.write('\n')

if __name__ == "__main__":
    main()
