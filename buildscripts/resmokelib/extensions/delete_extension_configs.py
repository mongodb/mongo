#!/usr/bin/env python3
import argparse
import logging
import os
import sys

import yaml

from buildscripts.resmokelib.extensions.generate_extension_configs import get_conf_out_dir


def _delete_generated_metrics_file(
    conf_path: str, conf_out_dir: str, logger: logging.Logger
) -> None:
    """Delete the metrics file a conf's extensionOptions points at, if we generated it.

    Only paths inside the conf output directory are removed, so an explicitly-configured
    metricsFilePath elsewhere is never touched.
    """
    try:
        with open(conf_path, "r") as f:
            conf = yaml.safe_load(f) or {}
    except (OSError, yaml.YAMLError):
        return
    metrics_path = (conf.get("extensionOptions") or {}).get("metricsFilePath")
    if not metrics_path:
        return
    if os.path.dirname(os.path.abspath(metrics_path)) != os.path.abspath(conf_out_dir):
        return
    try:
        os.remove(metrics_path)
        logger.info("Deleted extension metrics file %s", metrics_path)
    except FileNotFoundError:
        pass
    except OSError as e:
        logger.warning("Failed to delete extension metrics file %s: %s", metrics_path, e)


def delete_extension_configs(extension_names: str, logger: logging.Logger):
    """Delete extension .conf files and any generated metrics files they reference."""
    conf_out_dir = get_conf_out_dir()
    extension_names = [item.strip() for item in extension_names.split(",")]
    for name in extension_names:
        file_name = f"{name}.conf"
        file_path = os.path.join(conf_out_dir, file_name)
        _delete_generated_metrics_file(file_path, conf_out_dir, logger)
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
