#!/usr/bin/env python3
import argparse
import glob
import logging
import os
import sys
import uuid
from typing import Dict, Optional

from buildscripts.resmokelib.extensions.constants import (
    EVERGREEN_SEARCH_DIRS,
    LOCAL_SEARCH_DIRS,
)
from buildscripts.resmokelib.extensions.generate_extension_configs import generate_extension_configs


def find_and_generate_extension_configs(
    is_evergreen: bool,
    logger: logging.Logger,
    with_suffix: str,
    mongod_options: Optional[Dict] = None,
    mongos_options: Optional[Dict] = None,
) -> str:
    """Find extensions, generate .conf files, and add them to mongod/mongos startup parameters if specified."""
    search_dirs = EVERGREEN_SEARCH_DIRS if is_evergreen else LOCAL_SEARCH_DIRS

    logger.info("Extension search directories (in order): %s", search_dirs)
    ext_dir = next((d for d in search_dirs if os.path.isdir(d)), None)
    if not ext_dir:
        error_msg = f"No extension directories found in {search_dirs}. If extensions are required, ensure they are properly built."
        logger.error(error_msg)
        raise RuntimeError(error_msg)

    logger.info("Looking for extensions in %s", ext_dir)
    pattern = "*_mongo_extension.so"
    so_files = sorted(glob.glob(os.path.join(ext_dir, pattern)))

    if not so_files:
        error_msg = f"No extension files matching {pattern} found under {ext_dir}"
        logger.error(error_msg)
        raise RuntimeError(error_msg)

    logger.info("Found extension files: %s", so_files)
    # TODO SERVER-110317: Pass through extension names instead of paths to mongod/mongos.
    generate_extension_configs(so_files, logger, with_suffix)

    so_files = [
        f"{root}_{with_suffix}{ext}"
        for so_file in so_files
        for root, ext in [os.path.splitext(so_file)]
    ]
    joined_files = ",".join(so_files)
    if mongod_options is not None:
        mongod_options["loadExtensions"] = joined_files
    if mongos_options is not None:
        mongos_options["loadExtensions"] = joined_files
    return joined_files


def main():
    """Main execution function for command-line execution in Evergreen."""
    parser = argparse.ArgumentParser(
        description="Find MongoDB test extensions and generate .conf files."
    )
    parser.add_argument(
        "--expansions-file",
        type=str,
        help="File to output extension paths as a comma separated list.",
        default="../expansions.yml",
    )
    parser.add_argument(
        "--skip",
        type=lambda x: (str(x).lower() == "true"),
        default=False,
        help="If 'true', skip finding extensions and generating configuration files.",
    )

    args = parser.parse_args()
    logging.basicConfig(stream=sys.stdout, level=logging.INFO)
    logger = logging.getLogger(__name__)

    # Ensure the expansions file exists, even if we skip the main logic.
    with open(args.expansions_file, "a"):
        pass

    if args.skip or sys.platform != "linux":
        logger.info("Skipping task 'find_and_generate_extension_configs'.")
        sys.exit(0)

    try:
        joined_files = find_and_generate_extension_configs(
            is_evergreen=True, logger=logger, with_suffix=uuid.uuid4().hex
        )
        with open(args.expansions_file, "w+") as outfile:
            outfile.write(f"extension_paths: {joined_files}\n")
            logger.info("Wrote extension paths to expansions file %s", args.expansions_file)
    except RuntimeError:
        sys.exit(1)


if __name__ == "__main__":
    main()
