"""Extracts `mongo-debugsymbols.tgz`."""

import glob
import logging
import os
import shutil
import sys
import tarfile
import time

from buildscripts.resmokelib import config
from buildscripts.resmokelib.run import compare_start_time
from buildscripts.resmokelib.setup_multiversion.setup_multiversion import SetupMultiversion

_DEBUG_FILE_BASE_NAMES = ['mongo', 'mongod', 'mongos']


def extract_debug_symbols(root_logger):
    """
    Extract debug symbols. Idempotent.

    :param root_logger: logger to use
    :return: None
    """
    sym_files = []
    try:
        sym_files = _extract_tar(root_logger)
        # Try to copy the files without downloading first in case they already exist
        # pylint: disable=broad-except
    except Exception:
        pass

    if len(sym_files) < len(_DEBUG_FILE_BASE_NAMES):
        download_symbols_from_patch_build(root_logger)
        try:
            _extract_tar(root_logger)
        # We never want this to cause the whole task to fail.
        # The rest of the hang analyzer will continue to work without the
        # symbols it just won't be quite as helpful.
        # pylint: disable=broad-except
        except Exception as exception:
            root_logger.warning('Error extracting debug symbols: %s', exception)


def _extract_tar(root_logger):
    sym_files = []
    for (src, dest) in _extracted_files_to_copy():
        sym_files.append(dest)
        if os.path.exists(dest):
            root_logger.debug('Debug symbol %s already exists, not copying from %s.', dest, src)
            continue
        shutil.copy(src, dest)
        root_logger.debug('Copied debug symbol %s.', dest)
    return sym_files


def _extracted_files_to_copy():
    out = []
    for ext in ['debug', 'dSYM', 'pdb']:
        for file in _DEBUG_FILE_BASE_NAMES:
            haystack = os.path.join('dist-test', 'bin', '{file}.{ext}'.format(file=file, ext=ext))
            for needle in glob.glob(haystack):
                out.append((needle, os.path.join(os.getcwd(), os.path.basename(needle))))
    return out


def download_symbols_from_patch_build(root_logger):
    """Download debug symbol from patch build."""
    if config.DEBUG_SYMBOL_PATCH_URL is None:
        root_logger.info("Skipping downloading debug symbols since DEBUG_SYMBOLS_PATCH_URL is None")
        return

    retry_secs = 10

    while True:
        try:
            if config.DEBUG_SYMBOL_PATCH_URL is not None:
                SetupMultiversion.setup_mongodb(artifacts_url=None, binaries_url=None,
                                                symbols_url=config.DEBUG_SYMBOL_PATCH_URL,
                                                install_dir=os.getcwd())

            break

        except tarfile.ReadError:
            root_logger.info("Debug symbols unavailable after %s secs, retrying in %s secs",
                             compare_start_time(time.time()), retry_secs)
            time.sleep(retry_secs)

        ten_min = 10 * 60
        if compare_start_time(time.time()) > ten_min:
            root_logger.info(
                'Debug-symbols archive-file does not exist after %s secs; '
                'Hang-Analyzer may not complete successfully. Download URL: %s', ten_min,
                config.DEBUG_SYMBOL_PATCH_URL)
            break
