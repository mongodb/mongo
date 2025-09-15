#!/usr/bin/env python3
import argparse
import logging
import os
import shutil
import sys

import yaml

from buildscripts.resmokelib.extensions.constants import (
    CONF_IN_PATH,
    CONF_OUT_DIR,
)


def generate_extension_configs(so_files: list[str], logger: logging.Logger) -> list[str]:
    """Generate a .conf file for each extension .so file specified."""
    try:
        with open(CONF_IN_PATH, "r") as fstream:
            yml = yaml.safe_load(fstream)
            logger.info("Loaded test extensions' configuration file %s", CONF_IN_PATH)
    except FileNotFoundError as e:
        raise RuntimeError(f"Cannot find test extensions' configuration file {CONF_IN_PATH}") from e

    extensions = yml.get("extensions") or {}
    extension_names = []

    # Create a clean output directory.
    os.makedirs(CONF_OUT_DIR, exist_ok=True)

    for so_file in so_files:
        # path/to/libfoo_mongo_extension.so -> libfoo_mongo_extension
        file_name = os.path.basename(so_file)
        extension_name = os.path.splitext(file_name)[0]

        # TODO SERVER-110317: Remove 'lib' prefix and '_mongo_extension' suffix from extension names.

        # Add the parsed extension name to the list.
        extension_names.append(extension_name)

        conf_file_path = os.path.join(CONF_OUT_DIR, f"{extension_name}.conf")
        try:
            # Create the .conf file for the extension.
            with open(conf_file_path, "w+") as conf_file:
                # All extension .conf files will have a sharedLibraryPath.
                conf_file.write(f"sharedLibraryPath: {so_file}\n")

                # Copy over extensionOptions if they exist.
                if ext_config := extensions.get(extension_name):
                    yaml.dump(ext_config, conf_file)

                logger.info(
                    "Created .conf file for extension %s at %s",
                    extension_name,
                    conf_file_path,
                )
        except (IOError, OSError) as e:
            # Clean up created directories on failure.
            shutil.rmtree(CONF_OUT_DIR)
            raise RuntimeError(
                f"Failed to create .conf file for extension {extension_name} at {conf_file_path}"
            ) from e

    return extension_names


def main():
    """Main execution function for command-line execution from jstests."""
    parser = argparse.ArgumentParser(
        description="Generate .conf files for a given list of MongoDB test extensions."
    )
    parser.add_argument(
        "--so-files",
        type=str,
        required=True,
        help="A comma-separated list of .so file paths for the extensions.",
    )

    args = parser.parse_args()
    logging.basicConfig(stream=sys.stdout, level=logging.INFO)
    logger = logging.getLogger(__name__)

    so_files_list = [item.strip() for item in args.so_files.split(",")]

    try:
        extension_names = generate_extension_configs(so_files=so_files_list, logger=logger)
        logger.info(f"Successfully generated configuration for extensions: {extension_names}")
    except RuntimeError as e:
        logger.error(f"An error occurred: {e}")
        sys.exit(1)

    sys.exit(0)


if __name__ == "__main__":
    main()
