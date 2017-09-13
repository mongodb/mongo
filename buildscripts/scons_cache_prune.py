#!/USSR/bin/python
# encoding: utf-8
"""
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

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger("scons.cache.prune.lru")

GIGBYTES = 1024*1024*1024

cache_item = collections.namedtuple("CacheContents", ["path", "time", "size"])


def collect_cache_contents(cache_path):
    # map folder names to timestamps
    contents = []
    total = 0

    # collect names of directories and creation times
    for name in os.listdir(cache_path):
        path = os.path.join(cache_path, name)

        if os.path.isdir(path):
            for file_name in os.listdir(path):
                file_path = os.path.join(path, file_name)
                if os.path.isdir(file_path):
                    logger.warning("cache item {0} is a directory and not a file. "
                                   "The cache may be currupt.".format(file_path))
                    continue

                item = cache_item(path=file_path,
                                  time=os.stat(file_path).st_atime,
                                  size=os.stat(file_path).st_size)

                total += item.size

                contents.append(item)

    return (total, contents)


def prune_cache(cache_path, cache_size_gb, clean_ratio):
    # This function is taken as is from waf, with the interface cleaned up and some minor
    # stylistic changes.

    cache_size = cache_size_gb * GIGBYTES

    (total_size, contents) = collect_cache_contents(cache_path)

    logger.info("cache size {0}, quota {1}".format(total_size, cache_size))

    if total_size >= cache_size:
        logger.info("trimming the cache since {0} > {0}".format(total_size, cache_size))

        # make a list to sort the folders' by timestamp
        contents.sort(key=lambda x: x.time, reverse=True)  # sort by timestamp

        # now that the contents of things to delete is sorted by timestamp in reverse order, we
        # just delete things until the total_size falls below the target cache size ratio.
        while total_size >= cache_size * clean_ratio:
            if len(contents) == 0:
                shutil.rmtree(cache_path)
                logger.error("cache size is over quota, and there are no files in "
                             "the queue to delete. Removed the entire cache.")
                return False

            # (file_name, _, size) = contents.pop()
            cache_item = contents.pop()
            to_remove = cache_item.path + ".del"
            try:
                os.rename(cache_item.path, to_remove)
            except:
                # another process may have already cleared the file.
                pass
            else:
                try:
                    os.remove(to_remove)
                    logger.info("removed file from cache: {0}".format(cache_item.path))
                    total_size -= cache_item.size
                except Exception as e:
                    # this should not happen, but who knows?
                    logger.error("error [{0}, {1}] removing file '{2}', "
                                 "please report this error".format(e, type(e), to_remove))

        logger.info("total cache size at the end of pruning: {0}".format(total_size))
        return True
    else:
        logger.info("cache size ({0}) is currently within boundaries".format(total_size))
        return True


def main():
    parser = argparse.ArgumentParser(description="SCons cache pruning tool")

    parser.add_argument("--cache-dir", "-d", default=None,
                        help="path to the cache directory.")
    parser.add_argument("--cache-size", "-s", default=200, type=int,
                        help="maximum size of cache in GB.")
    parser.add_argument("--prune-ratio", "-p", default=0.8, type=float,
                        help=("ratio (as 1.0 > x > 0) of total cache size to prune "
                              "to when cache exceeds quota."))
    parser.add_argument("--print-cache-dir", default=False, action="store_true")

    args = parser.parse_args()

    if args.cache_dir is None or not os.path.isdir(args.cache_dir):
        logger.error("must specify a valid cache path, [{0}]".format(args.cache_dir))
        exit(1)

    ok = prune_cache(cache_path=args.cache_dir,
                     cache_size_gb=args.cache_size,
                     clean_ratio=args.prune_ratio)

    if not ok:
        logger.error("encountered error cleaning the cache. exiting.")
        exit(1)

if __name__ == "__main__":
    main()
