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
import json
import logging
import os
import shutil
import subprocess
import tempfile
from typing import Dict, List, Optional

from py_common import disagg
from py_common.decode_opts import DecodeOptions


logger = logging.getLogger(__name__)


def extract_value(metadata: Dict, key: str, required: bool = True, default = None) -> Optional[int]:
    if key not in metadata:
        if required:
            raise ValueError(f'Missing {key} in metadata: {metadata}')
        return default

    val_wrapper = metadata[key].get('val', {})
    if val_wrapper is not None:
        [val] = val_wrapper.values()
        return val
    if required:
        raise ValueError(f'Missing {key} in metadata: {metadata}')
    return default


def parse_metadata(page: Dict) -> disagg.Metadata:
    lsn: int = page.get('lsn', {}).get('lsn')
    if lsn is None:
        raise ValueError("Missing 'lsn' in page")

    meta = page.get('metadata', {})
    flags = extract_value(meta, 'flags')

    return disagg.Metadata(
        lsn=lsn,
        page_id=extract_value(meta, 'page_id'),
        table_id=extract_value(meta, 'table_id'),
        base_lsn=extract_value(meta, 'base_lsn', required=False, default=0),
        backlink_lsn=extract_value(meta, 'backlink_lsn', required=False, default=0),
        delta=(flags == disagg.UpdateTypeFlags.UPDATE_TYPE_DELTA),
    )


def decrypt_page(page, page_metadata, keyfile):
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

    if not keyfile or not os.path.exists(keyfile):
        raise FileNotFoundError(
            "keyfile not found: Decryption requires the test_keyfile for the KEK."
        )

    # Validate mandatory fields and extract metadata.
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
            '--keyFile', keyfile,
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
            logger.debug(f'pagedecryptor stdout: {decrypt_result.stdout}')
            logger.debug(f'pagedecryptor stderr: {decrypt_result.stderr}')
        except subprocess.CalledProcessError as e:
            logger.error(f"Error decrypting page {page_id}: {e}")
            logger.error(f"pagedecryptor stderr: {e.stderr}")
            raise

        decrypted_bytes = temp_output.read()

    return decrypted_bytes

def process_disagg_table(disagg_table, opts: DecodeOptions) -> disagg.DisaggTableSummary:
    '''
    Extract pages as json objects from the GetTableAtLSN API on the Object Read Proxy.

    Each line is a list of pages for a given page_id.
    Each object is a page entry containing page log metadata (same as PALI get args), and the page
    contents as a byte array. The page contents are encrypted by default and require the
    pagedecryptor tool in order to be decrypted and then decoded.
    '''

    disagg_pages: List[List[disagg.DisaggPage]] = []

    for line in disagg_table:
        pages = json.loads(line.strip())
        pages = pages.get('entries', [])

        page_disagg_pages: List[disagg.DisaggPage] = []

        for page in pages:
            page_metadata = parse_metadata(page)

            if not page['entry']:
                logger.info(f'page_id: {page_metadata.page_id} - empty page')
                continue

            decrypted_page_bytes = decrypt_page(page, page_metadata, opts.keyfile)
            page_disagg_pages.append(disagg.DisaggPage(page_metadata,
                                                       decrypted_page_bytes))

        disagg_pages.append(page_disagg_pages)

    return disagg.process_disagg_pages(disagg_pages, opts)
