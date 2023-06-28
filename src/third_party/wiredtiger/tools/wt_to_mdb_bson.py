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

import bson, codecs, pprint, subprocess, sys, re
from enum import Enum

# This script is intended to parse the output of three wt util commands and convert MongoDB bson
# from hexadecimal or byte format into ascii.
#
# It currently works with the following wt util commands:
# - dump
# - verify
# - printlog
#
# Those tools each perform a different function, and their usages are varied as such the script
# needs to handle their output separately. Originally many scripts existed for this purpose, the
# intent of this script is to provide a single place to perform all bson conversions.
#
# This script takes input of two forms, either through stdin or the user can pass the wt util
# location and the filename and the script will execute the required wt util command. When running
# with -f the script must be executed in the same directory as the database.
#
# Some example usages are:
#    - ./wt -r dump -x file:foo.wt | ./wt_to_mdb_bson -m dump
#    - ./wt -r verify -d dump_pages file:bar.wt | ./wt_to_mdb_bson -m verify
#    - ./wt_to_mdb_bson -m dump -f ./wt file:foo.wt
#    - ./wt_to_mdb_bson -m printlog -f ./wt

# A basic enum to determine which mode we are operating in.
class Mode(Enum):
    DUMP = 1
    VERIFY = 2
    PRINTLOG = 3

# Decodes a MongoDB file into a readable format.
def util_usage():
    print("Usage: wt_to_mdb_bson -m {dump|verify|printlog} [-f] [path_to_wt] [uri]")
    print('\t-m the intended mode that the wt util operated in or will be executed using.')
    print('\t-f the location of the wt util.')
    sys.exit(1)

# BSON printer helper.
def print_bson(bson):
    return pprint.pformat(bson, indent=1).replace('\n', '\n\t  ')

# A utility function for converting verify byte output into parsible hex.
def convert_byte(inp):
    ret = ""
    idx = 0
    while True:
        if idx >= len(inp):
            break
        ch = inp[idx]
        if ord(ch) != 92:
            ret += ch
            idx += 1
            continue
        lookAhead = inp[idx+1]
        if ord(lookAhead) != 92:
            ret += ch + 'x'
            idx += 1
            continue
        ret += ch + ch
        idx += 2
    return codecs.escape_decode(ret)[0]

# Converts the output of ./verify -d dump_pages to bson.
def wt_verify_to_bson(wt_output):
    pattern = re.compile('V {(.*?)}$')
    for line in wt_output:
        print(line, end='')
        matches = pattern.findall(line.strip())
        if matches:
            obj = bson.decode_all(convert_byte(matches[0]))[0]
            print('\t  %s' % (print_bson(obj),))

# Converts the output of ./wt printlog -x -u.
# Doesn't convert hex keys as I don't think they're bson.
def wt_printlog_to_bson(wt_output):
    pattern_value = re.compile('value-hex\": \"(.*)\"')
    for line in wt_output:
        value_match = pattern_value.search(line)
        if value_match:
            value_hex_str = value_match.group(1)
            value_bytes = bytes.fromhex(value_hex_str)
            try:
                bson_obj = bson.decode_all(value_bytes)
                print('\t\"value-bson\":%s' % (print_bson(bson_obj),))
            except Exception as e:
                # If bsons don't appear to be printing uncomment this line for the error reason.
                #logging.error('Error at %s', 'division', exc_info=e)
                print('\t\"value-hex\": \"' + value_hex_str + '\"')
        else:
            print(line.rstrip())

# Navigate to the data section of the MongoDB file if it exists for ./wt dump.
def find_data_section(mdb_file_contents):
    for i in range(len(mdb_file_contents)):
        line = mdb_file_contents[i].strip()
        if line == 'Data':
            return i + 1

    # No data section was found, return an invalid index.
    return -1

# Decode the keys and values from hex format to a readable BSON format for ./wt dump.
def decode_data_section(mdb_file_contents, data_index):
    # Loop through the data section and increment by 2, since we parse the K/V pairs.
    for i in range(data_index, len(mdb_file_contents), 2):
        key = mdb_file_contents[i].strip()
        value = mdb_file_contents[i + 1].strip()

        byt = codecs.decode(value, 'hex')
        obj = bson.decode_all(byt)[0]

        print('Key:\t%s' % key)
        print('Value:\n\t%s' % (print_bson(obj),))

# Convert the output of ./wt -r dump -x to bson.
def wt_dump_to_bson(wt_output):
    # Dump the MongoDB file into hex format.
    mdb_file_contents = wt_output
    data_index = find_data_section(mdb_file_contents)
    if data_index > 0:
        decode_data_section(mdb_file_contents, data_index)
    else:
        print("Error: No data section was found in the file.")
        exit()

# Call the wt util if required.
def execute_wt(mode, wtpath, uri):
    if mode == Mode.DUMP:
        return subprocess.check_output(
            [wtpath, "-r", "dump", "-x", uri], universal_newlines=True).splitlines()
    elif mode == Mode.VERIFY:
        return subprocess.check_output(
            [wtpath, "-r", "verify", "-d", "dump_pages", uri], universal_newlines=True).splitlines()
    else:
        return subprocess.check_output(
            [wtpath, "-r", "-C", "log=(compressor=snappy,path=journal/)", "printlog", "-u", "-x"], universal_newlines=True).splitlines()

def main():
    if len(sys.argv) < 3:
        util_usage()
        exit()

    if sys.argv[1] != '-m':
        print('A mode must be specified with -m.')
        util_usage()

    mode_str = sys.argv[2]
    if mode_str == 'dump':
        mode = Mode.DUMP
    elif mode_str == 'verify':
        mode = Mode.VERIFY
    elif mode_str == 'printlog':
        mode = Mode.PRINTLOG
    else:
        print('Invalid mode specified.')
        util_usage()

    # Does the user plan on passing wt's location and a file?
    if len(sys.argv) > 3:
        if sys.argv[3] != '-f':
            print('Invalid option specified.')
            util_usage()
        uri = None if mode == Mode.PRINTLOG else sys.argv[5]
        wt_output = execute_wt(mode, sys.argv[4], uri)
    else:
        # Read in stdout to a string then pass it like the wt_output.
        wt_output = sys.stdin.readlines()

    if mode == Mode.DUMP:
        wt_dump_to_bson(wt_output)
    elif mode == Mode.VERIFY:
        wt_verify_to_bson(wt_output)
    else:
        wt_printlog_to_bson(wt_output)

if __name__ == "__main__":
    main()
