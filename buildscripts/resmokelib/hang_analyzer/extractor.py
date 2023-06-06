"""Extracts `mongo-debugsymbols.tgz` in an idempotent manner for performance."""
import glob
import os
import tarfile
import time

from buildscripts.resmokelib.setup_multiversion.download import DownloadError
from buildscripts.resmokelib.run import compare_start_time
from buildscripts.resmokelib.utils.filesystem import build_hygienic_bin_path
from buildscripts.resmokelib.symbolizer import Symbolizer

_DEBUG_FILE_BASE_NAMES = ['mongo', 'mongod', 'mongos']


def download_debug_symbols(root_logger, symbolizer: Symbolizer, retry_secs: int = 10,
                           download_timeout_secs: int = 10 * 60):
    """
    Extract debug symbols. Idempotent.

    :param root_logger: logger to use
    :param symbolizer: pre-configured instance of symbolizer for downloading symbols.
    :param retry_secs: seconds before retrying to download symbols
    :param download_timeout_secs: timeout in seconds before failing to download
    :return: None
    """

    # Check if the files are already there. They would be on *SAN builds.
    sym_files = _get_symbol_files()

    if len(sym_files) >= len(_DEBUG_FILE_BASE_NAMES):
        root_logger.info(
            "Skipping downloading debug symbols as there are already symbol files present")
        return

    while True:
        try:
            symbolizer.execute()
            root_logger.info("Debug symbols successfully downloaded")
            break
        except (tarfile.ReadError, DownloadError):
            root_logger.warn(
                "Debug symbols unavailable after %s secs, retrying in %s secs, waiting for a total of %s secs",
                compare_start_time(time.time()), retry_secs, download_timeout_secs)
        time.sleep(retry_secs)

        if compare_start_time(time.time()) > download_timeout_secs:
            root_logger.warn(
                'Debug-symbols archive-file does not exist after %s secs; '
                'Hang-Analyzer may not complete successfully.', download_timeout_secs)
            break


def _get_symbol_files():
    out = []
    for ext in ['debug', 'dSYM', 'pdb']:
        for file in _DEBUG_FILE_BASE_NAMES:
            haystack = build_hygienic_bin_path(child='{file}.{ext}'.format(file=file, ext=ext))
            for needle in glob.glob(haystack):
                out.append((needle, os.path.join(os.getcwd(), os.path.basename(needle))))
    return out
