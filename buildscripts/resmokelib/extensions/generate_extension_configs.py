#!/usr/bin/env python3
import argparse
import functools
import json
import logging
import os
import shutil
import sys

import yaml

from buildscripts.resmokelib.extensions.constants import CONF_IN_PATH

# The mongot extension's canonical name is pinned in this git-tracked file so the server (which
# compiles it into kMongotExtensionName) and resmoke agree on the value used on --loadExtensions.
_MONGOT_EXTENSION_NAME_JSON = os.path.join(
    os.path.dirname(__file__),
    "..",
    "..",
    "..",
    "src",
    "mongo",
    "db",
    "pipeline",
    "search",
    "mongot_extension_name.json",
)

# The mongot extension's .so is published/built as libmongot_extension.so (file stem "mongot"),
# independent of its canonical name; its .conf points there via sharedLibraryPath.
MONGOT_EXTENSION_SO_STEM = "mongot"
_MONGOT_EXTENSION_SO_BASENAMES = (
    f"lib{MONGOT_EXTENSION_SO_STEM}_mongo_extension.so",
    f"lib{MONGOT_EXTENSION_SO_STEM}_extension.so",
)


@functools.cache
def get_mongot_extension_name() -> str:
    """Return the mongot extension's canonical name, read lazily from the git-tracked JSON.

    Read on first use rather than at import time so resmoke invocations that never build a mongot
    fixture don't hard-depend on the file being present. Fixtures special-case this extension: it
    needs a runtime-computed mongotHost.
    """
    with open(_MONGOT_EXTENSION_NAME_JSON) as f:
        return json.load(f)["name"]


def get_conf_out_dir() -> str:
    """Return the directory for generated extension .conf files.

    Resolves the temp dir from the environment (TMPDIR/TEMP/TMP), falling back to "/tmp". This must
    stay in sync with getExtensionConfDir() in jstests/noPassthrough/libs/extension_helpers.js,
    which the test framework uses to tell the server where to read extension configs. The fallback
    is a literal "/tmp" to match that helper rather than tempfile.gettempdir() (which could resolve
    elsewhere, e.g. /var/tmp, and diverge). The env vars are checked explicitly so a TMPDIR adjusted
    by resmoke or other test setup is picked up.
    """
    tmpdir = os.environ.get("TMPDIR") or os.environ.get("TEMP") or os.environ.get("TMP") or "/tmp"
    return os.path.join(tmpdir, "mongo", "extensions")


def generate_extension_configs(
    so_files: list[str],
    with_suffix: str,
    logger: logging.Logger,
    manual_options: str | None = None,
    manual_options_by_file: dict[str, dict] | None = None,
) -> list[str]:
    """Generate a .conf file for each extension .so file specified.

    manual_options_by_file maps a so_file path to an extensionOptions dict for that file,
    taking precedence over both manual_options and configurations.yml.
    """
    manual_options_by_file = manual_options_by_file or {}
    extensions = {}
    parsed_manual_options = None

    if not so_files:
        # Nothing to generate; the caller only wants the output directory created (see
        # ensureExtensionConfDir() in extension_helpers.js). Skip reading options entirely so this
        # path doesn't depend on CONF_IN_PATH resolving from the current working directory.
        pass
    elif manual_options:
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

    # Create the output directory with restricted permissions so it is not world-writable. The
    # mode passed to makedirs() is ignored if the directory already exists, so chmod explicitly.
    conf_out_dir = get_conf_out_dir()
    os.makedirs(conf_out_dir, mode=0o700, exist_ok=True)
    os.chmod(conf_out_dir, 0o700)

    for so_file in so_files:
        file_name = os.path.basename(so_file)
        if file_name in _MONGOT_EXTENSION_SO_BASENAMES:
            # The mongot extension's name is pinned in mongot_extension_name.json and intentionally
            # decoupled from its .so filename, so take it from there rather than deriving it from
            # the filename. The server keys off the same pinned value (kMongotExtensionName).
            extension_name = get_mongot_extension_name()
        else:
            # path/to/libfoo_mongo_extension.so -> libfoo_mongo_extension -> foo
            extension_name = (
                os.path.splitext(file_name)[0].removeprefix("lib").removesuffix("_mongo_extension")
            )
        extension_name_with_suffix = f"{extension_name}_{with_suffix}"

        # Add the parsed extension name to the list.
        extension_names.append(extension_name_with_suffix)

        conf_file_path = os.path.join(conf_out_dir, f"{extension_name_with_suffix}.conf")
        try:
            # Create the .conf file for the extension.
            with open(conf_file_path, "w+") as conf_file:
                # All extension .conf files will have a sharedLibraryPath.
                conf_file.write(f"sharedLibraryPath: {so_file}\n")

                if so_file in manual_options_by_file:
                    yaml.dump({"extensionOptions": manual_options_by_file[so_file]}, conf_file)
                elif parsed_manual_options is not None:
                    # Write manual options as extensionOptions.
                    yaml.dump({"extensionOptions": parsed_manual_options}, conf_file)
                # Fallback to extensionOptions from configurations.yml for this extension.
                elif ext_config := extensions.get(extension_name):
                    yaml.dump(ext_config, conf_file)

            # Restrict permissions explicitly rather than relying on umask, since the server
            # rejects extension config files that are group- or other-writable.
            os.chmod(conf_file_path, 0o600)

            logger.info(
                "Created .conf file for extension %s at %s",
                extension_name,
                conf_file_path,
            )
        except (IOError, OSError) as e:
            # Clean up created directories on failure.
            shutil.rmtree(conf_out_dir)
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
        default="",
        help="A comma-separated list of .so file paths for the extensions. May be empty, in which "
        "case no .conf files are generated but the output directory is still created with "
        "restricted permissions.",
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

    so_files_list = [item.strip() for item in args.so_files.split(",") if item.strip()]

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
