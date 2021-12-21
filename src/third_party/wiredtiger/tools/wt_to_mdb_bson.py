#!/usr/bin/env python
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

import bson, codecs, pprint, subprocess, sys

# Decodes a MongoDB file into a readable format.
def util_usage():
    print("Usage: wt_to_mdb_bson <path_to_wt> filename")

# Navigate to the data section of the MongoDB file if it exists.
def find_data_section(mdb_file_contents):
    for i in range(len(mdb_file_contents)):
        line = mdb_file_contents[i].strip()
        if line == 'Data':
            return i + 1
    
    # No data section was found, return an invalid index.
    return -1

# Decode the keys and values from hex format to a readable BSON format.
def decode_data_section(mdb_file_contents, data_index):
    # Loop through the data section and increment by 2, since we parse the K/V pairs.
    for i in range(data_index, len(mdb_file_contents), 2):
        key = mdb_file_contents[i].strip()
        value = mdb_file_contents[i + 1].strip()

        byt = codecs.decode(value, 'hex')
        obj = bson.decode_all(byt)[0]

        print('Key:\t%s' % key)
        print('Value:\n\t%s' % (pprint.pformat(obj, indent=1).replace('\n', '\n\t'),))

def dump_mdb_file(wtpath, filename):
    # Dump the MongoDB file into hex format.
    mdb_hex = subprocess.check_output([wtpath, "dump", "-x", "file:" + filename], universal_newlines=True)

    mdb_file_contents = mdb_hex.splitlines()
    data_index = find_data_section(mdb_file_contents)
    if data_index > 0:
        decode_data_section(mdb_file_contents, data_index)
    else:
        print("Error: No data section was found in the file.")
        exit()        

if len(sys.argv) != 3:
    util_usage()
    exit()

wtpath = sys.argv[1]
filename = sys.argv[2]
dump_mdb_file(wtpath, filename)
