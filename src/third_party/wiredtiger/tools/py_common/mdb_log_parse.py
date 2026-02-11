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

import codecs
import io
import re
import json

from py_common import binary_data
from py_common.file_format import wtdecode_file_object

    
def process_logs(f, opts):
    '''
    Extract the byte dump from mongo or wiredtiger logs.
    '''
    first_line = f.readline()
    f.seek(0)
    if is_mongo_log(first_line):
        return process_mongod_log(f, opts)
    else:
        return process_wiredtiger_log(f, opts)

def is_mongo_log(line):
    return line and line.startswith('{')

def process_mongod_log(f, opts):
    byte_dump = extract_mongodb_log_hex(f, opts)
    
    b = binary_data.BinaryFile(io.BytesIO(byte_dump))
    wtdecode_file_object(b, opts, len(byte_dump))
    
def process_wiredtiger_log(f, opts):
    while True:
        byte_dump = encode_bytes(f, opts)
        if not byte_dump:
            break

        b = binary_data.BinaryFile(io.BytesIO(byte_dump))
        wtdecode_file_object(b, opts, len(byte_dump))

def extract_mongodb_log_hex(f, opts):
    """
    Extract hex dump from MongoDB log file containing checksum mismatch errors.
    Looks for __bm_corrupt_dump messages and extracts all hex chunks.
    Returns the bytes from the __first__ complete checksum mismatch found.
    """

    lines = f.readlines()
    current_chunks = []
    block_info = None

    for line_num, line in enumerate(lines):
        try:
            log_entry = json.loads(line)
            msg = log_entry.get('attr', {}).get('message', {})

            # Check if this is a corrupt dump message
            if isinstance(msg, dict) and '__bm_corrupt_dump' in msg.get('msg', ''):
                # Extract block info and hex data
                msg_text = msg.get('msg', '')

                # Parse the block info: {offset, size, checksum}: (chunk N of M): hexdata
                match = re.search(r'\{0:\s*(\d+),\s*(\d+),\s*(0x[0-9a-f]+)\}:\s*\(chunk\s+(\d+)\s+of\s+(\d+)\):\s*([0-9a-f\s]+)', msg_text)
                if match:
                    offset, size, checksum, chunk_num, total_chunks, hexdata = match.groups()
                    chunk_num = int(chunk_num)
                    total_chunks = int(total_chunks)

                    if chunk_num == 1:
                        # Start of a new block
                        if current_chunks and len(current_chunks) == block_info[4]:
                            # We have a complete previous block, return it
                            if (opts.debug):
                                print(f'Found complete checksum mismatch block: offset={block_info[0]}, size={block_info[1]}, checksum={block_info[2]}')
                            return b''.join(current_chunks)

                        # Reset for new block
                        current_chunks = []
                        block_info = (offset, size, checksum, chunk_num, total_chunks)
                        if (opts.debug):
                            print(f'Found checksum mismatch at line: {line_num} for block with address: offset {offset}, size {size}, checksum {checksum} ({total_chunks} chunks)')

                    # Add this chunk
                    hexdata_clean = re.sub(r'[^0-9a-f]', '', hexdata.lower())
                    if hexdata_clean:
                        current_chunks.append(codecs.decode(hexdata_clean, 'hex'))

                    # Check if block is complete
                    if len(current_chunks) == total_chunks:
                        if (opts.debug):
                            print(f'Complete block collected: {len(current_chunks)} chunks')
                        return b''.join(current_chunks)
        except json.JSONDecodeError:
            # If we don't have a JSON log line, then this isn't a MongoDB log. Reset the file 
            # pointer to the start to read all the bytes again.
            f.seek(0)
            return encode_bytes(f, opts)
        except Exception as e:
            if opts.debug:
                print(f'Error parsing line {line_num}: {e}')

    # Return any incomplete block we collected
    if current_chunks:
        print(f'Warning: Returning incomplete block with {len(current_chunks)} chunks (expected {block_info[4] if block_info else "unknown"})')
        return b''.join(current_chunks)

    # No checksum mismatch found
    print('Error: No checksum mismatch found in log file')
    return bytearray()

def encode_bytes(f, opts):
    """
    Encode a text hex dump into raw bytes.

    Reads from the current file position only until a complete block is
    collected (as indicated by the chunk count), leaving the file pointer at
    the end of that block. All decoded chunks for the block are concatenated
    and returned as a single ``bytearray``.
    """
    allbytes = bytearray()
    expected_chunks = None

    for line in f:
        raw_line = line
        if ':' in line:
            (_, _, line) = line.rpartition(':')

        # Capture chunk position if present, e.g., "(chunk 1 of 3)".
        chunk_match = re.search(r"\(chunk\s+(\d+)\s+of\s+(\d+)\)", raw_line)
        current_chunk = None
        if chunk_match:
            current_chunk = int(chunk_match.group(1))
            expected_chunks = int(chunk_match.group(2))

        # Keep anything that looks like it could be hexadecimal,
        # remove everything else.
        nospace = re.sub(r"[^a-fA-F\d]", "", line)
        if opts.debug:
            print("LINE (len={}): {}".format(len(nospace), nospace))
        if len(nospace) > 0:
            if opts.debug:
                print("first={}, last={}".format(nospace[0], nospace[-1]))
            allbytes += codecs.decode(nospace, "hex")

            # If we do not know the total number of chunks, treat a single
            # decoded line as a complete block.
            if expected_chunks is None:
                break

        # Stop when we have consumed the declared final chunk.
        if expected_chunks is not None and current_chunk is not None and current_chunk >= expected_chunks:
            break

    return allbytes
