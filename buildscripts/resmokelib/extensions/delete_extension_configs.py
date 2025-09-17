#!/usr/bin/env python3
import argparse
import logging
import os
import sys

from buildscripts.resmokelib.extensions.constants import (
    CONF_OUT_DIR,
)


def delete_extension_configs(extension_names: str, logger: logging.Logger):
    """Delete extension .conf files."""
    extension_names = [item.strip() for item in extension_names.split(",")]
    for name in extension_names:
        file_name = f"{name}.conf"
        file_path = os.path.join(CONF_OUT_DIR, file_name)
        try:
            os.remove(file_path)
            logger.info("Deleted extension configuration file %s", file_path)
        except FileNotFoundError:
            logger.warning("Could not find extension configuration file to delete: %s", file_path)
        except OSError as e:
            raise RuntimeError(f"Failed to delete extension configuration file {file_path}") from e


def main():
    """Main execution function for command-line execution from jstests."""
    parser = argparse.ArgumentParser(
        description="Delete MongoDB test extension configuration files."
    )

    parser.add_argument(
        "--extension-names",
        type=str,
        required=True,
        help="A comma-separated list of extension names with .conf files to delete.",
    )

    args = parser.parse_args()
    logging.basicConfig(stream=sys.stdout, level=logging.INFO)
    logger = logging.getLogger(__name__)

    try:
        delete_extension_configs(logger=logger, extension_names=args.extension_names)
    except RuntimeError as e:
        logger.error(f"An error occurred: {e}")
        sys.exit(1)

    sys.exit(0)


if __name__ == "__main__":
    main()
