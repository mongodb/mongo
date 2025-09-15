#!/usr/bin/env python3
import argparse
import logging
import os
import sys

from buildscripts.resmokelib.extensions.constants import (
    CONF_OUT_DIR,
)


def delete_extension_configs(extension_paths: str, logger: logging.Logger):
    """Delete extension .conf files."""
    # TODO SERVER-110317: Change extension paths to extension names.
    extension_paths = [item.strip() for item in extension_paths.split(",")]
    for path in extension_paths:
        basename = os.path.splitext(os.path.basename(path))[0]
        file_name = f"{basename}.conf"
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
    # TODO SERVER-110317: Change extension paths to extension names.
    parser.add_argument(
        "--extension-paths",
        type=str,
        required=True,
        help="A comma-separated list of extension paths.",
    )

    args = parser.parse_args()
    logging.basicConfig(stream=sys.stdout, level=logging.INFO)
    logger = logging.getLogger(__name__)

    try:
        delete_extension_configs(logger=logger, extension_paths=args.extension_paths)
    except RuntimeError as e:
        logger.error(f"An error occurred: {e}")
        sys.exit(1)

    sys.exit(0)


if __name__ == "__main__":
    main()
