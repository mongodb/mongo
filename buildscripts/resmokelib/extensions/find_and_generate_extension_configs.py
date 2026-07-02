#!/usr/bin/env python3
import glob
import logging
import os
import uuid

from buildscripts.resmokelib.extensions.constants import (
    EVERGREEN_SEARCH_DIRS,
    LOCAL_SEARCH_DIRS,
)
from buildscripts.resmokelib.extensions.generate_extension_configs import (
    generate_extension_configs,
    get_conf_out_dir,
)


def normalize_load_extensions(load_extensions) -> list[str]:
    """Validate and normalize the load_extensions parameter.

    Ensures load_extensions is either None or a list and returns a de-duplicated list preserving
    the original order.
    """
    if load_extensions is None:
        return []
    if not isinstance(load_extensions, list):
        raise TypeError(
            f"load_extensions must be None or a list, got {type(load_extensions).__name__}: {load_extensions!r}"
        )
    # De-duplicate while preserving order.
    return list(dict.fromkeys(load_extensions))


def _get_extension_dir(is_evergreen: bool, logger: logging.Logger) -> str:
    """Return the first existing extension directory for the current environment."""
    search_dirs = EVERGREEN_SEARCH_DIRS if is_evergreen else LOCAL_SEARCH_DIRS

    logger.info("Extension search directories (in order): %s", search_dirs)
    ext_dir = next((d for d in search_dirs if os.path.isdir(d)), None)
    if not ext_dir:
        error_msg = f"No extension directories found in {search_dirs}. If extensions are required, ensure they are properly built."
        logger.error(error_msg)
        raise RuntimeError(error_msg)

    return ext_dir


def _append_to_load_extensions(options: dict, new_names: str):
    """Append extension names to an options dict's loadExtensions value without overwriting the existing value."""
    existing = options.get("loadExtensions", "")
    if existing:
        options["loadExtensions"] = f"{existing},{new_names}"
    else:
        options["loadExtensions"] = new_names


def _generate_and_append_to_load_extensions(
    so_files: list[str],
    logger: logging.Logger,
    mongod_options: dict,
    mongos_options: dict | None = None,
) -> str:
    extension_names = generate_extension_configs(so_files, uuid.uuid4().hex, logger)
    joined_names = ",".join(extension_names)

    _append_to_load_extensions(mongod_options, joined_names)
    conf_out_dir = get_conf_out_dir()
    mongod_options["extensionsConfigPath"] = conf_out_dir
    if mongos_options is not None:
        _append_to_load_extensions(mongos_options, joined_names)
        mongos_options["extensionsConfigPath"] = conf_out_dir

    return joined_names


def find_all_extension_so_files(
    is_evergreen: bool,
    logger: logging.Logger,
) -> list[str]:
    """Find extension .so files in the appropriate directories based on environment."""
    ext_dir = _get_extension_dir(is_evergreen, logger)

    logger.info("Looking for extensions in %s", ext_dir)
    pattern = "*_mongo_extension.so"
    so_files = sorted(glob.glob(os.path.join(ext_dir, pattern)))

    if not so_files:
        error_msg = f"No extension files matching {pattern} found under {ext_dir}"
        logger.error(error_msg)
        raise RuntimeError(error_msg)

    return so_files


def find_and_generate_all_extension_configs(
    is_evergreen: bool,
    logger: logging.Logger,
    mongod_options: dict,
    mongos_options: dict | None = None,
) -> str:
    """Find extensions, generate .conf files, and add them to mongod/mongos startup parameters if specified."""
    so_files = find_all_extension_so_files(is_evergreen, logger)

    logger.info("Found extension files: %s", so_files)

    return _generate_and_append_to_load_extensions(so_files, logger, mongod_options, mongos_options)


def find_and_generate_named_extension_configs(
    extension_names: list[str],
    is_evergreen: bool,
    logger: logging.Logger,
    mongod_options: dict,
    mongos_options: dict | None = None,
) -> str:
    """Find specific extensions by name, generate their .conf files, and append to loadExtensions. Unlike
    find_and_generate_all_extension_configs which discovers *all* extensions, this function loads only the
    explicitly listed extensions."""
    ext_dir = _get_extension_dir(is_evergreen, logger)
    all_so_files = []

    for name in extension_names:
        pattern = f"lib{name}_mongo_extension.so"
        so_files = sorted(glob.glob(os.path.join(ext_dir, pattern)))

        if not so_files:
            raise RuntimeError(
                f"Extension '{name}' not found: no files matching {pattern} in {ext_dir}"
            )
        if len(so_files) > 1:
            raise RuntimeError(
                f"Ambiguous extension '{name}': multiple files match {pattern} in {ext_dir}: {so_files}"
            )

        logger.info("Found extension file for '%s': %s", name, so_files[0])
        all_so_files.append(so_files[0])

    return _generate_and_append_to_load_extensions(
        all_so_files, logger, mongod_options, mongos_options
    )
