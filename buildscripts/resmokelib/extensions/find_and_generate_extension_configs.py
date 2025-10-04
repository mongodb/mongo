#!/usr/bin/env python3
import glob
import logging
import os
import uuid
from typing import Dict, Optional

from buildscripts.resmokelib.extensions.constants import (
    EVERGREEN_SEARCH_DIRS,
    LOCAL_SEARCH_DIRS,
)
from buildscripts.resmokelib.extensions.generate_extension_configs import generate_extension_configs


def find_extension_so_files(
    is_evergreen: bool,
    logger: logging.Logger,
) -> list[str]:
    """Find extension .so files in the appropriate directories based on environment."""
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

    return so_files


def find_and_generate_extension_configs(
    is_evergreen: bool,
    logger: logging.Logger,
    mongod_options: Dict,
    mongos_options: Optional[Dict] = None,
) -> str:
    """Find extensions, generate .conf files, and add them to mongod/mongos startup parameters if specified."""
    so_files = find_extension_so_files(is_evergreen, logger)

    logger.info("Found extension files: %s", so_files)
    extension_names = generate_extension_configs(so_files, uuid.uuid4().hex, logger)
    joined_names = ",".join(extension_names)

    mongod_options["loadExtensions"] = joined_names

    if mongos_options is not None:
        mongos_options["loadExtensions"] = joined_names
    return joined_names
