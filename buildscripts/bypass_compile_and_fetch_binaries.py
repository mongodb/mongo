#!/usr/bin/env python3
"""Bypass compile and fetch binaries."""

import json
import logging
import os
import sys
import tarfile
from tempfile import TemporaryDirectory
import urllib.error
import urllib.parse
import urllib.request

import click

from evergreen.api import RetryingEvergreenApi
from git.repo import Repo
import requests
import structlog
from structlog.stdlib import LoggerFactory
import yaml

EVG_CONFIG_FILE = ".evergreen.yml"

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# pylint: disable=wrong-import-position
from buildscripts.ciconfig.evergreen import parse_evergreen_file
# pylint: enable=wrong-import-position

structlog.configure(logger_factory=LoggerFactory())
LOGGER = structlog.get_logger(__name__)

_IS_WINDOWS = (sys.platform == "win32" or sys.platform == "cygwin")

# If changes are only from files in the bypass_files list or the bypass_directories list, then
# bypass compile, unless they are also found in the BYPASS_EXTRA_CHECKS_REQUIRED lists. All other
# file changes lead to compile.
BYPASS_WHITELIST = {
    "files": {
        "etc/evergreen.yml",
    },
    "directories": {
        "buildscripts/",
        "jstests/",
        "pytests/",
    },
}  # yapf: disable

# These files are exceptions to any whitelisted directories in bypass_directories. Changes to
# any of these files will disable compile bypass. Add files you know should specifically cause
# compilation.
BYPASS_BLACKLIST = {
    "files": {
        "buildscripts/errorcodes.py",
        "buildscripts/make_archive.py",
        "buildscripts/moduleconfig.py",
        "buildscripts/msitrim.py",
        "buildscripts/packager_enterprise.py",
        "buildscripts/packager.py",
        "buildscripts/scons.py",
        "buildscripts/utils.py",
    },
    "directories": {
        "buildscripts/idl/",
        "src/",
    }
}  # yapf: disable

# Changes to the BYPASS_EXTRA_CHECKS_REQUIRED_LIST may or may not allow bypass compile, depending
# on the change. If a file is added to this list, the _check_file_for_bypass() function should be
# updated to perform any extra checks on that file.
BYPASS_EXTRA_CHECKS_REQUIRED = {
    "etc/evergreen.yml",
}  # yapf: disable

# Expansions in etc/evergreen.yml that must not be changed in order to bypass compile.
EXPANSIONS_TO_CHECK = {
    "compile_flags",
}  # yapf: disable


def executable_name(pathname):
    """Return the executable name."""
    # Ensure that executable files on Windows have a ".exe" extension.
    if _IS_WINDOWS and os.path.splitext(pathname)[1] != ".exe":
        return "{}.exe".format(pathname)
    return pathname


def archive_name(archive):
    """Return the archive name."""
    # Ensure the right archive extension is used for Windows.
    if _IS_WINDOWS:
        return "{}.zip".format(archive)
    return "{}.tgz".format(archive)


def requests_get_json(url):
    """Return the JSON response."""
    response = requests.get(url)
    response.raise_for_status()

    try:
        return response.json()
    except ValueError:
        LOGGER.warning("Invalid JSON object returned with response", response=response.text)
        raise


def write_out_bypass_compile_expansions(patch_file, **expansions):
    """Write out the macro expansions to given file."""
    with open(patch_file, "w") as out_file:
        LOGGER.info("Saving compile bypass expansions", patch_file=patch_file,
                    expansions=expansions)
        yaml.safe_dump(expansions, out_file, default_flow_style=False)


def write_out_artifacts(json_file, artifacts):
    """Write out the JSON file with URLs of artifacts to given file."""
    with open(json_file, "w") as out_file:
        LOGGER.info("Generating artifacts.json from pre-existing artifacts", json=json.dumps(
            artifacts, indent=4))
        json.dump(artifacts, out_file)


def _create_bypass_path(prefix, build_id, name):
    """
    Create the path for the bypass expansions.

    :param prefix: Prefix of the path.
    :param build_id: Build-Id to use.
    :param name: Name of file.
    :return: Path to use for bypass expansion.
    """
    return archive_name(f"{prefix}/{name}-{build_id}")


def generate_bypass_expansions(project, build_variant, revision, build_id):
    """
    Create a dictionary of the generate bypass expansions.

    :param project: Evergreen project.
    :param build_variant: Build variant being run in.
    :param revision: Revision to use in expansions.
    :param build_id: Build id to use in expansions.
    :returns: Dictionary of expansions to update.
    """
    prefix = f"{project}/{build_variant}/{revision}"

    return {
        # With compile bypass we need to update the URL to point to the correct name of the base
        # commit binaries.
        "mongo_binaries": _create_bypass_path(prefix, build_id, "binaries/mongo"),
        # With compile bypass we need to update the URL to point to the correct name of the base
        # commit debug symbols.
        "mongo_debugsymbols": _create_bypass_path(prefix, build_id, "debugsymbols/debugsymbols"),
        # With compile bypass we need to update the URL to point to the correct name of the base
        # commit mongo shell.
        "mongo_shell": _create_bypass_path(prefix, build_id, "binaries/mongo-shell"),
        # Enable bypass compile
        "bypass_compile": True,
    }


def _get_original_etc_evergreen(path):
    """
    Get the etc/evergreen configuration before the changes were made.

    :param path: path to etc/evergreen.
    :return: An EvergreenProjectConfig for the previous etc/evergreen file.
    """
    repo = Repo(".")
    previous_contents = repo.git.show([f"HEAD:{path}"])
    with TemporaryDirectory() as tmpdir:
        file_path = os.path.join(tmpdir, "evergreen.yml")
        with open(file_path, "w") as fp:
            fp.write(previous_contents)
        return parse_evergreen_file(file_path)


def _check_etc_evergreen_for_bypass(path, build_variant):
    """
    Check if changes to etc/evergreen can be allowed to bypass compile.

    :param path: Path to etc/evergreen file.
    :param build_variant: Build variant to check.
    :return: True if changes can bypass compile.
    """
    variant_before = _get_original_etc_evergreen(path).get_variant(build_variant)
    variant_after = parse_evergreen_file(path).get_variant(build_variant)

    for expansion in EXPANSIONS_TO_CHECK:
        if variant_before.expansion(expansion) != variant_after.expansion(expansion):
            return False

    return True


def _check_file_for_bypass(file, build_variant):
    """
    Check if changes to the given file can be allowed to bypass compile.

    :param file: File to check.
    :param build_variant: Build Variant to check.
    :return: True if changes can bypass compile.
    """
    if file == "etc/evergreen.yml":
        return _check_etc_evergreen_for_bypass(file, build_variant)

    return True


def _file_in_group(filename, group):
    """
    Determine if changes to the given filename require compile to be run.

    :param filename: Filename to check.
    :param group: Dictionary containing files and filename to check.
    :return: True if compile should be run for filename.
    """
    if "files" not in group:
        raise TypeError("No list of files to check.")
    if filename in group["files"]:
        return True

    if "directories" not in group:
        raise TypeError("No list of directories to check.")
    if any(filename.startswith(directory) for directory in group["directories"]):
        return True

    return False


def should_bypass_compile(patch_file, build_variant):
    """
    Determine whether the compile stage should be bypassed based on the modified patch files.

    We use lists of files and directories to more precisely control which modified patch files will
    lead to compile bypass.
    :param patch_file: A list of all files modified in patch build.
    :param build_variant: Build variant where compile is running.
    :returns: True if compile should be bypassed.
    """
    with open(patch_file, "r") as pch:
        for filename in pch:
            filename = filename.rstrip()
            # Skip directories that show up in 'git diff HEAD --name-only'.
            if os.path.isdir(filename):
                continue

            log = LOGGER.bind(filename=filename)
            if _file_in_group(filename, BYPASS_BLACKLIST):
                log.warning("Compile bypass disabled due to blacklisted file")
                return False

            if not _file_in_group(filename, BYPASS_WHITELIST):
                log.warning("Compile bypass disabled due to non-whitelisted file")
                return False

            if filename in BYPASS_EXTRA_CHECKS_REQUIRED:
                if not _check_file_for_bypass(filename, build_variant):
                    log.warning("Compile bypass disabled due to extra checks for file.")
                    return False

    return True


def find_build_for_previous_compile_task(evergreen_api, revision, project, build_variant):
    """
    Find build_id of the base revision.

    :param evergreen_api: Evergreen.py object.
    :param revision: The base revision being run against.
    :param project: The evergreen project.
    :param build_variant: The build variant whose artifacts we want to use.
    :return: build_id of the base revision.
    """
    project_prefix = project.replace("-", "_")
    version_of_base_revision = "{}_{}".format(project_prefix, revision)
    version = evergreen_api.version_by_id(version_of_base_revision)
    build_id = version.build_by_variant(build_variant).id
    return build_id


def find_previous_compile_task(evergreen_api, build_id, revision):
    """
    Find compile task that should be used for skip compile..

    :param evergreen_api: Evergreen.py object.
    :param build_id: Build id of the desired compile task.
    :param revision: The base revision being run against.
    :return: Evergreen.py object containing data about the desired compile task.
    """
    index = build_id.find(revision)
    compile_task_id = "{}compile_{}".format(build_id[:index], build_id[index:])
    task = evergreen_api.task_by_id(compile_task_id)
    return task


@click.command()
@click.option("--project", required=True, help="The evergreen project.")
@click.option("--build-variant", required=True,
              help="The build variant whose artifacts we want to use.")
@click.option("--revision", required=True, help="Base revision of the build.")
@click.option("--patch-file", required=True, help="A list of all files modified in patch build.")
@click.option("--out-file", required=True, help="File to write expansions to.")
@click.option("--json-artifact", required=True,
              help="The JSON file to write out the metadata of files to attach to task.")
def main(  # pylint: disable=too-many-arguments,too-many-locals,too-many-statements
        project, build_variant, revision, patch_file, out_file, json_artifact):
    """
    Create a file with expansions that can be used to bypass compile.

    If for any reason bypass compile is false, we do not write out the expansion. Only if we
    determine to bypass compile do we write out the expansions.
    \f

    :param project: The evergreen project.
    :param build_variant: The build variant whose artifacts we want to use.
    :param revision: Base revision of the build.
    :param patch_file: A list of all files modified in patch build.
    :param out_file: File to write expansions to.
    :param json_artifact: The JSON file to write out the metadata of files to attach to task.
    """
    logging.basicConfig(
        format="[%(asctime)s - %(name)s - %(levelname)s] %(message)s",
        level=logging.DEBUG,
        stream=sys.stdout,
    )

    # Determine if we should bypass compile based on modified patch files.
    if should_bypass_compile(patch_file, build_variant):
        evergreen_api = RetryingEvergreenApi.get_api(config_file=EVG_CONFIG_FILE)
        build_id = find_build_for_previous_compile_task(evergreen_api, revision, project,
                                                        build_variant)
        if not build_id:
            LOGGER.warning("Could not find build id. Default compile bypass to false.",
                           revision=revision, project=project)
            return
        task = find_previous_compile_task(evergreen_api, build_id, revision)
        if task is None or not task.is_success():
            LOGGER.warning(
                "Could not retrieve artifacts because the compile task for base commit"
                " was not available. Default compile bypass to false.", task_id=task.task_id)
            return
        LOGGER.info("Fetching pre-existing artifacts from compile task", task_id=task.task_id)
        artifacts = []
        for artifact in task.artifacts:
            filename = os.path.basename(artifact.url)
            if filename.startswith(build_id):
                LOGGER.info("Retrieving archive", filename=filename)
                # This is the artifacts.tgz as referenced in evergreen.yml.
                try:
                    urllib.request.urlretrieve(artifact.url, filename)
                except urllib.error.ContentTooShortError:
                    LOGGER.warning(
                        "The artifact could not be completely downloaded. Default"
                        " compile bypass to false.", filename=filename)
                    return

                # Need to extract certain files from the pre-existing artifacts.tgz.
                extract_files = [
                    executable_name("mongobridge"),
                    executable_name("mongoebench"),
                    executable_name("mongoed"),
                    executable_name("wt"),
                ]
                with tarfile.open(filename, "r:gz") as tar:
                    # The repo/ directory contains files needed by the package task. May
                    # need to add other files that would otherwise be generated by SCons
                    # if we did not bypass compile.
                    subdir = [
                        tarinfo for tarinfo in tar.getmembers()
                        if tarinfo.name.startswith("repo/") or tarinfo.name in extract_files
                    ]
                    LOGGER.info("Extracting the files...", filename=filename,
                                files="\n".join(tarinfo.name for tarinfo in subdir))
                    tar.extractall(members=subdir)
            elif filename.startswith("mongo-src"):
                LOGGER.info("Retrieving mongo source", filename=filename)
                # This is the distsrc.[tgz|zip] as referenced in evergreen.yml.
                try:
                    urllib.request.urlretrieve(artifact.url, filename)
                except urllib.error.ContentTooShortError:
                    LOGGER.warn(
                        "The artifact could not be completely downloaded. Default"
                        " compile bypass to false.", filename=filename)
                    return
                extension = os.path.splitext(filename)[1]
                distsrc_filename = "distsrc{}".format(extension)
                LOGGER.info("Renaming", filename=filename, rename=distsrc_filename)
                os.rename(filename, distsrc_filename)
            else:
                LOGGER.info("Linking base artifact to this patch build", filename=filename)
                # For other artifacts we just add their URLs to the JSON file to upload.
                files = {
                    "name": artifact.name,
                    "link": artifact.url,
                    "visibility": "private",
                }
                # Check the link exists, else raise an exception. Compile bypass is disabled.
                requests.head(artifact.url).raise_for_status()
                artifacts.append(files)

        # SERVER-21492 related issue where without running scons the jstests/libs/key1
        # and key2 files are not chmod to 0600. Need to change permissions here since we
        # bypass SCons.
        os.chmod("jstests/libs/key1", 0o600)
        os.chmod("jstests/libs/key2", 0o600)
        os.chmod("jstests/libs/keyForRollover", 0o600)

        # This is the artifacts.json file.
        write_out_artifacts(json_artifact, artifacts)

        # Need to apply these expansions for bypassing SCons.
        expansions = generate_bypass_expansions(project, build_variant, revision, build_id)
        write_out_bypass_compile_expansions(out_file, **expansions)


if __name__ == "__main__":
    main()  # pylint: disable=no-value-for-parameter
