"""Extracts `mongo-debugsymbols.tgz`."""

import glob
import os
import sys


def extract_debug_symbols(root_logger):
    """
    Extract debug symbols. Idempotent.

    :param root_logger: logger to use
    :return: None
    """
    path = os.path.join(os.getcwd(), 'mongo-debugsymbols.tgz')
    root_logger.debug('Starting: Extract debug-symbols from %s.', path)
    if not os.path.exists(path):
        root_logger.info('Debug-symbols archive-file does not exist. '
                         'Hang-Analyzer may not complete successfully, '
                         'or debug-symbols may already be extracted.')
        return
    try:
        _extract_tar(path, root_logger)
        root_logger.debug('Finished: Extract debug-symbols from %s.', path)
    # We never want this to cause the whole task to fail.
    # The rest of the hang analyzer will continue to work without the
    # symbols it just won't be quite as helpful.
    # pylint: disable=broad-except
    except Exception as exception:
        root_logger.warning('Error when extracting %s: %s', path, exception)


def _extract_tar(path, root_logger):
    import shutil
    # The file name is always .tgz but it's "secretly" a zip file on Windows :(
    compressed_format = 'zip' if sys.platform == "win32" else 'gztar'
    shutil.unpack_archive(path, format=compressed_format)
    for (src, dest) in _extracted_files_to_copy():
        if os.path.exists(dest):
            root_logger.debug('Debug symbol %s already exists, not copying from %s.', dest, src)
            continue
        shutil.copy(src, dest)
        root_logger.debug('Copied debug symbol %s.', dest)


def _extracted_files_to_copy():
    out = []
    for ext in ['debug', 'dSYM', 'pdb']:
        for file in ['mongo', 'mongod', 'mongos']:
            haystack = os.path.join('dist-test', 'bin', '{file}.{ext}'.format(file=file, ext=ext))
            for needle in glob.glob(haystack):
                out.append((needle, os.path.join(os.getcwd(), os.path.basename(needle))))
    return out
