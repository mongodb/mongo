#!/USSR/bin/python
# encoding: utf-8
"""
Prune the scons cache.

This script, borrowed from some waf code, with a stand alone interface, provides a way to
remove files from the cache on an LRU (least recently used) basis to prevent the scons cache
from outgrowing the storage capacity.
"""

# Inspired by: https://github.com/krig/waf/blob/master/waflib/extras/lru_cache.py
# Thomas Nagy 2011

import argparse
import collections
import logging
import os
import shutil

LOGGER = logging.getLogger("scons.cache.prune.lru")  # type: ignore

GIGBYTES = 1024 * 1024 * 1024

CacheItem = collections.namedtuple("CacheContents", ["path", "time", "size"])


def get_cachefile_size(file_path, is_cksum):
    """Get the size of the cachefile."""
    if is_cksum:
        size = 0
        for cksum_path in os.listdir(file_path):
            cksum_path = os.path.join(file_path, cksum_path)
            size += os.stat(cksum_path).st_size
    else:
        size = os.stat(file_path).st_size
    return size


def collect_cache_contents(cache_path):
    """Collect the cache contents."""
    # map folder names to timestamps
    contents = []
    total = 0

    # collect names of directories and creation times
    for name in os.listdir(cache_path):
        path = os.path.join(cache_path, name)

        if os.path.isdir(path):
            for file_name in os.listdir(path):
                file_path = os.path.join(path, file_name)
                # Cache prune script is allowing only directories with this extension
                # which comes from the validate_cache_dir.py tool in SCons, it must match
                # the extension set in that file.
                cksum_type = False
                if os.path.isdir(file_path):
                    hash_length = -32
                    tmp_length = -len('.cksum.tmp') + hash_length
                    cksum_type = (file_path.lower().endswith('.cksum')
                                  or file_path.lower().endswith('.del')
                                  or file_path.lower()[tmp_length:hash_length] == '.cksum.tmp')

                    if not cksum_type:
                        LOGGER.warning(
                            "cache item %s is a directory and not a file. "
                            "The cache may be corrupt.", file_path)
                        continue

                try:

                    item = CacheItem(path=file_path, time=os.stat(file_path).st_atime,
                                     size=get_cachefile_size(file_path, cksum_type))

                    total += item.size

                    contents.append(item)
                except OSError as err:
                    LOGGER.warning("Ignoring error querying file %s : %s", file_path, err)

    return (total, contents)


def prune_cache(cache_path, cache_size_gb, clean_ratio):
    """Prune the cache."""
    # This function is taken as is from waf, with the interface cleaned up and some minor
    # stylistic changes.

    cache_size = cache_size_gb * GIGBYTES

    (total_size, contents) = collect_cache_contents(cache_path)

    LOGGER.info("cache size %d, quota %d", total_size, cache_size)

    if total_size >= cache_size:
        LOGGER.info("trimming the cache since %d > %d", total_size, cache_size)

        # make a list to sort the folders' by timestamp
        contents.sort(key=lambda x: x.time, reverse=True)  # sort by timestamp

        # now that the contents of things to delete is sorted by timestamp in reverse order, we
        # just delete things until the total_size falls below the target cache size ratio.
        while total_size >= cache_size * clean_ratio:
            if not contents:
                LOGGER.error("cache size is over quota, and there are no files in "
                             "the queue to delete.")
                return False

            cache_item = contents.pop()

            # check the atime again just to make sure something wasn't accessed while
            # we pruning other files.
            if cache_item.time < os.stat(cache_item.path).st_atime:
                continue

            to_remove = cache_item.path + ".del"
            try:
                os.rename(cache_item.path, to_remove)
            except Exception as err:  # pylint: disable=broad-except
                # another process may have already cleared the file.
                LOGGER.warning("Unable to rename %s : %s", cache_item, err)
            else:
                try:
                    if os.path.isdir(to_remove):
                        shutil.rmtree(to_remove)
                    else:
                        os.remove(to_remove)
                    total_size -= cache_item.size
                except Exception as err:  # pylint: disable=broad-except
                    # this should not happen, but who knows?
                    LOGGER.error("error [%s, %s] removing file '%s', "
                                 "please report this error", err, type(err), to_remove)

        LOGGER.info("total cache size at the end of pruning: %d", total_size)
        return True
    LOGGER.info("cache size (%d) is currently within boundaries", total_size)
    return True


def main():
    """Execute Main entry."""

    logging.basicConfig(level=logging.INFO)

    parser = argparse.ArgumentParser(description="SCons cache pruning tool")

    parser.add_argument("--cache-dir", "-d", default=None, help="path to the cache directory.")
    parser.add_argument("--cache-size", "-s", default=200, type=int,
                        help="maximum size of cache in GB.")
    parser.add_argument(
        "--prune-ratio", "-p", default=0.8, type=float,
        help=("ratio (as 1.0 > x > 0) of total cache size to prune "
              "to when cache exceeds quota."))
    parser.add_argument("--print-cache-dir", default=False, action="store_true")

    args = parser.parse_args()

    if args.cache_dir is None or not os.path.isdir(args.cache_dir):
        LOGGER.error("must specify a valid cache path, [%s]", args.cache_dir)
        exit(1)

    ok = prune_cache(cache_path=args.cache_dir, cache_size_gb=args.cache_size,
                     clean_ratio=args.prune_ratio)

    if not ok:
        LOGGER.error("encountered error cleaning the cache. exiting.")
        exit(1)


if __name__ == "__main__":
    main()
