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


def generate_extension_configs(
    so_files: list[str],
    with_suffix: str,
    logger: logging.Logger,
    manual_options: str | None = None,
) -> list[str]:
    """Generate a .conf file for each extension .so file specified."""
    extensions = {}
    parsed_manual_options = None

    if manual_options:
        if len(so_files) > 1:
            raise RuntimeError(
                "When using --manual-options, only one .so file can be specified via --so-files"
            )
        try:
            parsed_manual_options = yaml.safe_load(manual_options)
            logger.info("Using manual extension options supplied via --manual-options")
        except yaml.YAMLError as e:
            raise RuntimeError(f"Failed to parse manual options as YAML: {manual_options}") from e
    else:
        # Load configurations.yml if manual options were not provided.
        try:
            with open(CONF_IN_PATH, "r") as fstream:
                yml = yaml.safe_load(fstream)
                logger.info("Loaded test extensions' configuration file %s", CONF_IN_PATH)
        except FileNotFoundError as e:
            raise RuntimeError(
                f"Cannot find test extensions' configuration file {CONF_IN_PATH}"
            ) from e
        extensions = (yml or {}).get("extensions") or {}

    extension_names = []

    # Create a clean output directory.
    os.makedirs(CONF_OUT_DIR, exist_ok=True)

    for so_file in so_files:
        # path/to/libfoo_mongo_extension.so -> libfoo_mongo_extension
        file_name = os.path.basename(so_file)
        extension_name = os.path.splitext(file_name)[0]

        # libfoo_mongo_extension -> foo
        extension_name = extension_name.removeprefix("lib").removesuffix("_mongo_extension")
        extension_name_with_suffix = f"{extension_name}_{with_suffix}"

        # Add the parsed extension name to the list.
        extension_names.append(extension_name_with_suffix)

        conf_file_path = os.path.join(CONF_OUT_DIR, f"{extension_name_with_suffix}.conf")
        try:
            # Create the .conf file for the extension.
            with open(conf_file_path, "w+") as conf_file:
                # All extension .conf files will have a sharedLibraryPath.
                conf_file.write(f"sharedLibraryPath: {so_file}\n")

                if parsed_manual_options is not None:
                    # Write manual options as extensionOptions.
                    yaml.dump({"extensionOptions": parsed_manual_options}, conf_file)
                # Fallback to extensionOptions from configurations.yml for this extension.
                elif ext_config := extensions.get(extension_name):
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
    parser.add_argument(
        "--with-suffix",
        type=str,
        required=True,
        help="A string to add to the end of every generated .conf file.",
    )
    parser.add_argument(
        "--manual-options",
        dest="manual_options",
        type=str,
        help="Manual extensionOptions to include instead of from configurations.yml (YAML or JSON string).",
    )

    args = parser.parse_args()
    logging.basicConfig(stream=sys.stdout, level=logging.INFO)
    logger = logging.getLogger(__name__)

    so_files_list = [item.strip() for item in args.so_files.split(",")]

    try:
        extension_names = generate_extension_configs(
            so_files=so_files_list,
            with_suffix=args.with_suffix,
            logger=logger,
            manual_options=(args.manual_options or None),
        )
        logger.info(f"Successfully generated configuration for extensions: {extension_names}")
    except RuntimeError as e:
        logger.error(f"An error occurred: {e}")
        sys.exit(1)

    sys.exit(0)


if __name__ == "__main__":
    main()
