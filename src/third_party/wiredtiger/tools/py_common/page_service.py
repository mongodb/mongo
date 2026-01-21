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

# Data structures for decoding pages from the page service.
import base64
from dataclasses import dataclass
import enum
import io
import json
import logging
import os
import shutil
import subprocess
import sys
import tempfile
from typing import Any, Dict, List, Optional

from py_common import binary_data, btree_format
from py_common.printer import Printer


logger = logging.getLogger(__name__)

class UpdateTypeFlags(enum.IntFlag):
    '''
    Update types as defined in page_service.proto
    '''
    UPDATE_TYPE_DELTA = 0
    UPDATE_TYPE_FULL_IMAGE = 1
    UPDATE_TYPE_TOMBSTONE = 2
    UPDATE_TYPE_CHECKPOINT_END = 3

class PageLogMetadata():
    lsn: int
    page_id: int
    table_id: int
    base_lsn: Optional[int]
    backlink_lsn: Optional[int]
    flags: int
    
    def __init__(self, log_entry: Dict[str, Any]):
        # Extract LSN
        self.lsn: int = log_entry.get("lsn", {}).get("lsn")
        if self.lsn is None:
            raise ValueError(f"Missing 'lsn' in page")

        # Extract Metadata Fields
        meta = log_entry.get("metadata", {})
        
        # Mandatory fields
        self.page_id = self._extract_value_required(meta, "page_id")
        self.table_id = self._extract_value_required(meta, "table_id")
        self.flags = self._extract_value_required(meta, "flags")
            
        # Optional fields
        self.base_lsn: Optional[int] = self._extract_value(meta, "base_lsn")
        self.backlink_lsn: Optional[int] = self._extract_value(meta, "backlink_lsn")

    def _extract_value(self, metadata: Dict, key: str) -> Optional[int]:
        '''
        Extracts an optional value from the json object returning None if it does not exist.
        '''
        if key not in metadata:
            return None
            
        val_wrapper = metadata[key].get("val", {})
        
        # We take the first value found in the wrapper 
        if val_wrapper is not None:
            [val] = val_wrapper.values()
            return val
        return None
    
    def _extract_value_required(self, metadata: Dict, key: str) -> int:
        '''
        Extracts a value from the json object raising an error if it doesn't exist.
        '''
        if key not in metadata:
            raise ValueError(f"Missing {key} in metadata: {metadata}")
            
        val_wrapper = metadata[key].get("val", {})
        
        # We take the first value found in the wrapper 
        if val_wrapper is not None:
            [val] = val_wrapper.values()
            return val
        raise ValueError(f"Missing {key} in metadata: {metadata}")
    
    def is_delta(self):
        return self.flags == UpdateTypeFlags.UPDATE_TYPE_DELTA
    
    # The turtle metadata page has a set table_id of 1. Defined in meta.h
    def is_metadata_page(self):
        return self.table_id == 1
    
    def __str__(self):
        meta_string = (
            f"Disagg Page Metadata:\n"
            f"  page_id: {self.page_id}\n"
            f"  table_id: {self.table_id}\n"
            f"  lsn: {self.lsn}\n"
            f"  base_lsn: {self.base_lsn if self.base_lsn is not None else 'None'}\n"
            f"  backlink_lsn: {self.backlink_lsn if self.backlink_lsn is not None else 'None'}"
        )
        
        return meta_string
    
@dataclass
class DisaggTableSummary:
    """
    Summary statistics for the decoded B-tree.
    """
    total_pages: int = 0
    delta_pages: int = 0
    full_pages: int = 0
    
    def update_with_page(self, page: PageLogMetadata):
        self.total_pages += 1
        if page.is_delta():
            self.delta_pages += 1
        else:
            self.full_pages += 1
            
def decrypt_page(page, page_metadata, opts):
    """
    Call the pagedecryptor tool from the mongo repo.
    """
    
    if not shutil.which('pagedecryptor'):
        raise FileNotFoundError(
            "pagedecryptor not found: Decryption requires the 'pagedecryptor' tool from the MongoDB "
            "encryption module. Please install the tool and ensure the 'pagedecryptor' binary is on your "
            "PATH.\n"
            "Hint - compile from the mongo repo with:\n"
            "bazel build //src/mongo/db/modules/atlas/src/disagg_storage/encryption:pagedecryptor"
        )
        
    if not opts.keyfile or not os.path.exists(opts.keyfile):
        raise FileNotFoundError(
            "keyfile not found: Decryption requires the test_keyfile for the KEK."
        )
    
    # Validate mandatory fields and extract metadata using PageLogMetadata
    page_id = page_metadata.page_id
    lsn = page_metadata.lsn
    table_id = page_metadata.table_id
    base_lsn = page_metadata.base_lsn
    backlink_lsn = page_metadata.backlink_lsn
    
    with tempfile.NamedTemporaryFile(mode='w', delete=True, suffix='.in') as temp_input, \
         tempfile.NamedTemporaryFile(mode='rb', delete=True, suffix='.out') as temp_output:
    
        # Write the page bytes to the temporary input file. The decrypt tool expects a file containing a
        # base64 encoded byte string.
        entry_bytes = page.get('entry', [])
        raw_bytes = bytes(entry_bytes)
        base64_encoded = base64.b64encode(raw_bytes)
        temp_input.write(base64_encoded.decode('ascii'))
        temp_input.flush()
        
        cmd = [
            'pagedecryptor',
            '--inputPath', temp_input.name,
            '--outputPath', temp_output.name,
            '--keyFile', opts.keyfile,
            '--lsn', str(lsn),
            '--tableId', str(table_id),
            '--pageId', str(page_id),
        ]
        
        if base_lsn is not None:
            cmd.extend(['--baseLsn', str(base_lsn)])
        if backlink_lsn is not None:
            cmd.extend(['--backlinkLsn', str(backlink_lsn)])
            
        if page_metadata.is_delta():
            cmd.extend(['--isDelta'])
        
        try:
            decrypt_result = subprocess.run(cmd, capture_output=True, text=True, check=True)
            if (opts.debug):
                print(f'pagedecryptor stdout: {decrypt_result.stdout}')
                print(f'pagedecryptor stderr: {decrypt_result.stderr}')
        except subprocess.CalledProcessError as e:
            print(f"Error decrypting page {page_id}: {e}", file=sys.stderr)
            print(f"pagedecryptor stderr: {e.stderr}", file=sys.stderr)
            raise
        
        decrypted_bytes = temp_output.read()
    
    return decrypted_bytes

def extract_disagg_pages(disagg_table, opts) -> DisaggTableSummary:
    '''
    Extract pages a json objects from the GetTableAtLSN API on the Object Read Proxy.
    
    Each line is is a list of pages for a given page_id.
    Each object is a page entry containing page log metadata (same as PALI get args), and the page
    contents as a byte array. The page contents are encrypted by default and require the
    pagedecryptor tool in order to be decrypted and then decoded.
    '''
    
    table_summary = DisaggTableSummary()
    
    for line in disagg_table:
        # Parse each line as a separate json object containing the page entries associated with a 
        # page id.
        pages = json.loads(line.strip())
        page_entries = pages.get('entries', [])
        
        # Keep track of delta chains to verify them.
        delta_chain: List[btree_format.WTPage] = []

        # Each page_id can have a number of entries associated with it. 
        for page_entry in page_entries:
            page_metadata = PageLogMetadata(page_entry)
            
            # If the page entry is empty do not try and decrypt/decode it. SLS sometimes stores
            # empty pages.
            if not page_entry['entry']:
                logger.info(f'page_id: {page_metadata.page_id} - empty page')
                continue
            
            decrypted_page_bytes = decrypt_page(page_entry, page_metadata, opts)
            b = binary_data.BinaryFile(io.BytesIO(decrypted_page_bytes))
            p = Printer(b, opts)
            
            p.rint(page_metadata)
            
            # The disagg metadata page is plaintext, print it as such.
            if page_metadata.is_metadata_page():
                p.rint('Disagg Metadata File:')
                page_string = decrypted_page_bytes.decode('ascii')
                p.rint(f'  {page_string}')
                
                # The metadata table root page address cookie is stored as plaintext hex in the addr 
                # field of the checkpoint string. Extract it and decode it to print the disagg
                # page metadata.
                p.rint('Metadata Table Root Page:')
                addr_string = page_string.split('addr="')[1].split('"')[0]
                addr = btree_format.DisaggAddr.parse(bytes.fromhex(addr_string))
                p.rint(addr)
                p.rint('')
                continue
                
            page = btree_format.WTPage()
            page = page.parse(b, len(decrypted_page_bytes), opts)
        
            # Verification checks between the block/page headers and the page service metadata.

            # Deltas are sorted from most recent to oldest in the GetTableAtLSN response.
            if page_metadata.is_delta():
                # Make sure the block header and page service metadata agree that this is a delta
                # page.
                if page.block_header.magic != btree_format.BlockDisaggHeader.WT_BLOCK_DISAGG_MAGIC_DELTA:
                    logger.error(f"Delta page has incorrect block flag: {page.block_header.magic}")
                    
                # Verify the current delta page against the previous delta page.
                if delta_chain:
                    prev_page = delta_chain[-1]
                    
                    # Checksums are stored between delta pages to verify we're reading the correct
                    # page.
                    prev_checksum = prev_page.block_header.previous_checksum
                    if prev_checksum != page.block_header.checksum:
                        logger.error(f"Checksum mismatch between delta pages. Previous checksum: {prev_checksum}, current checksum: {page.block_header.checksum}")
                        
                    # Write generation numbers should be monotonically increasing.
                    if prev_page.page_header.write_gen <= page.page_header.write_gen:
                        logger.error(f"Write generation detected out-of-order between pages: page {page}, previous page: {prev_page}")
                        
                    
                delta_chain.append(page)
            # Verification checks for full pages.
            else:
                # Make sure the block header and page service metadata agree that this is a full
                # page.
                if page.block_header.magic != btree_format.BlockDisaggHeader.WT_BLOCK_DISAGG_MAGIC_BASE:
                    logger.error(f"Full page has incorrect block flag: {page.block_header.magic}")
                    
                # Reset the delta chain list.
                delta_chain = []
                
            # Update the table summary statistics.
            table_summary.update_with_page(page_metadata)
            page.print_page(opts)
            p.rint('')

    return table_summary
