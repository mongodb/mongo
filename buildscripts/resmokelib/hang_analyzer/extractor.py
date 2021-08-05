"""Extracts `mongo-debugsymbols.tgz` in an idempotent manner for performance."""
import glob
import os
import tarfile
import time

from buildscripts.resmokelib.setup_multiversion.download import DownloadError
from buildscripts.resmokelib.run import compare_start_time
from buildscripts.resmokelib.utils.filesystem import build_hygienic_bin_path

_DEBUG_FILE_BASE_NAMES = ['mongo', 'mongod', 'mongos']


def download_debug_symbols(root_logger, symbolizer):
    """
    Extract debug symbols. Idempotent.

    :param root_logger: logger to use
    :param symbolizer: pre-configured instance of symbolizer for downloading symbols.
    :return: None
    """
    retry_secs = 10

    # Check if the files are already there. They would be on *SAN builds.
    sym_files = _get_symbol_files()

    if len(sym_files) >= len(_DEBUG_FILE_BASE_NAMES):
        root_logger.info(
            "Skipping downloading debug symbols as there are already symbol files present")
        return

    while True:
        try:
            symbolizer.execute()
            break
        except (tarfile.ReadError, DownloadError):
            root_logger.info("Debug symbols unavailable after %s secs, retrying in %s secs",
                             compare_start_time(time.time()), retry_secs)
        time.sleep(retry_secs)

        ten_min = 10 * 60
        if compare_start_time(time.time()) > ten_min:
            root_logger.info(
                'Debug-symbols archive-file does not exist after %s secs; '
                'Hang-Analyzer may not complete successfully.', ten_min)
            break


def _get_symbol_files():
    out = []
    for ext in ['debug', 'dSYM', 'pdb']:
        for file in _DEBUG_FILE_BASE_NAMES:
            haystack = build_hygienic_bin_path(child='{file}.{ext}'.format(file=file, ext=ext))
            for needle in glob.glob(haystack):
                out.append((needle, os.path.join(os.getcwd(), os.path.basename(needle))))
    return out
