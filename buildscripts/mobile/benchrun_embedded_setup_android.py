#!/usr/bin/env python3
"""Setup an Android device to run the benchrun_embedded test suite."""

import glob
import logging
import optparse
import os
import posixpath
import shutil
import sys
import tarfile
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

# pylint: disable=wrong-import-position
# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from buildscripts.mobile import adb_monitor

# Initialize the global logger.
LOGGER = logging.getLogger(__name__)


def download_and_untar(url, root_dir):
    """Download url and untar into root_dir."""
    temp_file = tempfile.NamedTemporaryFile(delete=False, suffix=".tgz").name
    LOGGER.info("Downloading %s", url)
    urllib.request.urlretrieve(url, temp_file)
    with tarfile.open(temp_file, "r:gz") as tar:
        tar.extractall(root_dir)
    os.remove(temp_file)


def push_directory_contents(adb, local_dir, remote_dir):
    """Push contents of local_dir to remote_dir via adb."""
    # Push the contents of temp_dir.
    paths = glob.glob(os.path.join(local_dir, "*"))
    paths.sort()
    paths_msg = paths
    if isinstance(paths, list):
        paths_msg = [os.path.basename(path) for path in paths]
        paths_msg = "{}{}".format(paths_msg[:5], "" if len(paths) <= 5 else " ...")
    LOGGER.info("Pushing %s to %s", paths_msg, remote_dir)
    adb.push(paths, remote_dir)


def download_and_push(adb, url, remote_dir, local_dir=None):
    """Download url and push directory to remote_dir via adb.

    If local_dir is defined, then save the unzipped tar there.
    """
    temp_dir = tempfile.mkdtemp()
    download_and_untar(url, temp_dir)
    push_directory_contents(adb, temp_dir, remote_dir)
    if local_dir:
        if os.path.exists(local_dir):
            LOGGER.info("Removing local path %s", local_dir)
            shutil.rmtree(local_dir)
        LOGGER.info("Saving local copy to %s", local_dir)
        shutil.move(temp_dir, local_dir)
    else:
        shutil.rmtree(temp_dir)


def create_empty_remote_dirs(adb, dirs):
    """Create empty remote directories via adb."""
    # We can specify dirs as a single directory name or as list.
    if isinstance(dirs, str):
        dirs = [dirs]
    # Keep directories in order, so we do not delete a root level later.
    dirs.sort()
    for remote_dir in dirs:
        LOGGER.info("Creating remote directory %s", remote_dir)
        adb.shell(
            "if [ -d {remote_dir} ]; then rm -fr {remote_dir}; fi; mkdir -p {remote_dir}".format(
                remote_dir=remote_dir))


def move_sdk_files(adb, sdk_root_dir):
    """Move all the files in bin and lib into sdk_root_dir."""
    LOGGER.info("Moving SDK bin & lib files to %s", sdk_root_dir)
    adb_command = "lib_dir=$(find {} -name 'lib')".format(sdk_root_dir)
    adb_command = "{}; bin_dir=$(find {} -name 'bin')".format(adb_command, sdk_root_dir)
    adb_command = "{}; mv $lib_dir/* $bin_dir/* {}".format(adb_command, sdk_root_dir)
    adb.shell(adb_command)


def main():
    """Execute Main program."""

    logging.basicConfig(format="%(asctime)s %(levelname)s %(message)s", level=logging.INFO)
    logging.Formatter.converter = time.gmtime

    benchrun_root = "/data/local/tmp/benchrun_embedded"

    parser = optparse.OptionParser()
    program_options = optparse.OptionGroup(parser, "Program Options")
    device_options = optparse.OptionGroup(parser, "Device Options")
    sdk_options = optparse.OptionGroup(parser, "Embedded Test SDK Options")
    json_options = optparse.OptionGroup(parser, "JSON benchrun file Options")

    program_options.add_option("--adbBinary", dest="adb_binary",
                               help="The path for adb. Defaults to '%default', which is in $PATH.",
                               default="adb")

    device_options.add_option(
        "--rootRemoteDir", dest="embedded_root_dir",
        help="The remote root directory to store the files. Defaults to '%default'.",
        default=benchrun_root)

    device_options.add_option(
        "--dbDir", dest="db_dir",
        help=("The remote dbpath directory used by mongoebench."
              " Will be created if it does not exist. Defaults to '%default'."),
        default=posixpath.join(benchrun_root, "db"))

    device_options.add_option(
        "--resultsDir", dest="results_dir",
        help=("The remote directory to store the mongoebench results."
              " Will be created if it does not exist. Defaults to '%default'."),
        default=posixpath.join(benchrun_root, "results"))

    device_options.add_option(
        "--sdkRemoteDir", dest="sdk_remote_dir",
        help="The remote directory to store the embedded SDK files. Defaults to '%default'.",
        default=posixpath.join(benchrun_root, "sdk"))

    device_options.add_option(
        "--benchrunJsonRemoteDir", dest="json_remote_dir",
        help="The remote directory to store the benchrun JSON files."
        " Defaults to '%default'.", default=posixpath.join(benchrun_root, "testcases"))

    sdk_url = "https://s3.amazonaws.com/mciuploads/mongodb-mongo-master/embedded-sdk-test/embedded-sdk-android-arm64-latest.tgz"
    sdk_options.add_option(
        "--sdkUrl", dest="sdk_url",
        help=("The embedded SDK test URL. This tarball must contain mongoebench and"
              " any required shared object (.so) libraries. Defaults to '%default'."),
        default=sdk_url)

    sdk_options.add_option(
        "--sdkLocalDir", dest="sdk_local_dir",
        help="The local directory of embedded SDK files to be copied."
        "If specified, overrides --sdkUrl.", default=None)

    sdk_options.add_option(
        "--sdkSaveLocalDir", dest="sdk_save_local_dir",
        help=("The local directory to save the downloaded embedded SDK as an unzipped tarball."
              " Only used if the embedded SDK tarball is downloaded. Note - this will delete"
              " the existing directory."), default=None)

    json_url = "https://s3.amazonaws.com/mciuploads/mongodb-mongo-master/benchrun_embedded/benchrun_json_files.tgz"
    json_options.add_option(
        "--benchrunJsonUrl", dest="json_url",
        help=("The benchrun JSON files URL. This tarball must contain all the JSON"
              " files to be used in the benchrun embedded test."
              " Defaults to '%default'."), default=json_url)

    json_options.add_option(
        "--benchrunJsonLocalDir", dest="json_local_dir",
        help="The local directory of benchrun JSON files to be copied."
        "If specified, overrides --benchrunJsonUrl.", default=None)

    json_options.add_option(
        "--benchrunJsonSaveLocalDir", dest="json_save_local_dir",
        help=("The local directory to save the downloaded benchrun JSON as an unzipped tarball."
              " Only used if the benchrun JSON files tarball is downloaded. Note - this will"
              " delete the existing directory.  Defaults to '%default'."), default=os.path.join(
                  "benchrun_embedded", "testcases"))

    json_options.add_option(
        "--noBenchrunJsonSaveLocal", action="store_true", dest="no_json_save_local_dir",
        help=("Disable saving downloaded benchrun JSON as an unzipped tarball."), default=False)

    parser.add_option_group(program_options)
    parser.add_option_group(device_options)
    parser.add_option_group(sdk_options)
    parser.add_option_group(json_options)
    options, _ = parser.parse_args()

    if options.no_json_save_local_dir:
        options.json_save_local_dir = None

    adb = adb_monitor.Adb(options.adb_binary)
    adb.device_available()
    LOGGER.info("Detected devices by adb:\n%s%s", adb.devices(), adb.device_available())

    # Create/empty remote directories.
    create_empty_remote_dirs(adb, [
        options.embedded_root_dir, options.db_dir, options.results_dir, options.sdk_remote_dir,
        options.json_remote_dir
    ])

    # Download, untar and push Embedded SDK Tests & Benchrun JSON files.
    # Unfortunately gunzip may not exist on the Android device, so we cannot use this remote command:
    #   curl URL | tar -xzv -C LOCAL_DIR

    if options.sdk_local_dir:
        push_directory_contents(adb, options.sdk_local_dir, options.sdk_remote_dir)
    else:
        download_and_push(adb, options.sdk_url, options.sdk_remote_dir, options.sdk_save_local_dir)
    move_sdk_files(adb, options.sdk_remote_dir)

    if options.json_local_dir:
        push_directory_contents(adb, options.json_local_dir, options.json_remote_dir)
    else:
        download_and_push(adb, options.json_url, options.json_remote_dir,
                          options.json_save_local_dir)


if __name__ == "__main__":
    main()
