#!/usr/bin/env python3
"""Bypass compile and fetch binaries."""

import argparse
import json
import os
import re
import sys
import tarfile
import urllib.error
import urllib.parse
import urllib.request

# pylint: disable=ungrouped-imports
try:
    from urllib.parse import urlparse
except ImportError:
    from urllib.parse import urlparse  # type: ignore
# pylint: enable=ungrouped-imports

import requests
import yaml

_IS_WINDOWS = (sys.platform == "win32" or sys.platform == "cygwin")


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
        print("Invalid JSON object returned with response: {}".format(response.text))
        raise


def read_evg_config():
    """Attempt to parse the Evergreen configuration from its home location.

    Return None if the configuration file wasn't found.
    """
    evg_file = os.path.expanduser("~/.evergreen.yml")
    if os.path.isfile(evg_file):
        with open(evg_file, "r") as fstream:
            return yaml.safe_load(fstream)

    return None


def write_out_bypass_compile_expansions(patch_file, **expansions):
    """Write out the macro expansions to given file."""
    with open(patch_file, "w") as out_file:
        print("Saving compile bypass expansions to {0}: ({1})".format(patch_file, expansions))
        yaml.safe_dump(expansions, out_file, default_flow_style=False)


def write_out_artifacts(json_file, artifacts):
    """Write out the JSON file with URLs of artifacts to given file."""
    with open(json_file, "w") as out_file:
        print("Generating artifacts.json from pre-existing artifacts {0}".format(
            json.dumps(artifacts, indent=4)))
        json.dump(artifacts, out_file)


def generate_bypass_expansions(project, build_variant, revision, build_id):
    """Perform the generate bypass expansions."""
    expansions = {}
    # With compile bypass we need to update the URL to point to the correct name of the base commit
    # binaries.
    expansions["mongo_binaries"] = (archive_name("{}/{}/{}/binaries/mongo-{}".format(
        project, build_variant, revision, build_id)))

    # With compile bypass we need to update the URL to point to the correct name of the base commit
    # debug symbols.
    expansions["mongo_debugsymbols"] = (archive_name("{}/{}/{}/debugsymbols/debugsymbols-{}".format(
        project, build_variant, revision, build_id)))

    # With compile bypass we need to update the URL to point to the correct name of the base commit
    # mongo shell.
    expansions["mongo_shell"] = (archive_name("{}/{}/{}/binaries/mongo-shell-{}".format(
        project, build_variant, revision, build_id)))

    # Enable bypass compile
    expansions["bypass_compile"] = True
    return expansions


def should_bypass_compile():
    """Determine whether the compile stage should be bypassed based on the modified patch files.

    We use lists of files and directories to more precisely control which modified patch files will
    lead to compile bypass.
    """

    # If changes are only from files in the bypass_files list or the bypass_directories list, then
    # bypass compile, unless they are also found in the requires_compile_directories lists. All
    # other file changes lead to compile.
    # Add files to this list that should not cause compilation.
    bypass_files = [
        "etc/evergreen.yml",
    ]

    # Add directories to this list that should not cause compilation.
    bypass_directories = [
        "buildscripts/",
        "jstests/",
        "pytests/",
    ]

    # These files are exceptions to any whitelisted directories in bypass_directories. Changes to
    # any of these files will disable compile bypass. Add files you know should specifically cause
    # compilation.
    requires_compile_files = [
        "buildscripts/errorcodes.py",
        "buildscripts/make_archive.py",
        "buildscripts/moduleconfig.py",
        "buildscripts/msitrim.py",
        "buildscripts/packager_enterprise.py",
        "buildscripts/packager.py",
        "buildscripts/scons.py",
        "buildscripts/utils.py",
    ]

    # These directories are exceptions to any whitelisted directories in bypass_directories and will
    # disable compile bypass. Add directories you know should specifically cause compilation.
    requires_compile_directories = [
        "buildscripts/idl/",
        "src/",
    ]

    args = parse_args()

    with open(args.patchFile, "r") as pch:
        for filename in pch:
            filename = filename.rstrip()
            # Skip directories that show up in 'git diff HEAD --name-only'.
            if os.path.isdir(filename):
                continue

            if (filename in requires_compile_files or any(
                    filename.startswith(directory) for directory in requires_compile_directories)):
                print("Compile bypass disabled after detecting {} as being modified because"
                      " it is a file known to affect compilation.".format(filename))
                return False

            if (filename not in bypass_files
                    and not any(filename.startswith(directory)
                                for directory in bypass_directories)):
                print("Compile bypass disabled after detecting {} as being modified because"
                      " it isn't a file known to not affect compilation.".format(filename))
                return False
    return True


def parse_args():
    """Parse the program arguments."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--project", required=True,
                        help="The Evergreen project. e.g mongodb-mongo-master")

    parser.add_argument("--buildVariant", required=True,
                        help="The build variant. e.g enterprise-rhel-62-64-bit")

    parser.add_argument("--revision", required=True, help="The base commit hash.")

    parser.add_argument("--patchFile", required=True,
                        help="A list of all files modified in patch build.")

    parser.add_argument("--outFile", required=True,
                        help="The YAML file to write out the macro expansions.")

    parser.add_argument("--jsonArtifact", required=True,
                        help="The JSON file to write out the metadata of files to attach to task.")

    return parser.parse_args()


def main():  # pylint: disable=too-many-locals,too-many-statements
    """Execute Main entry.

    From the /rest/v1/projects/{project}/revisions/{revision} endpoint find an existing build id
    to generate the compile task id to use for retrieving artifacts when bypassing compile.

    We retrieve the URLs to the artifacts from the task info endpoint at
    /rest/v1/tasks/{build_id}. We only download the artifacts.tgz and extract certain files
    in order to retain any modified patch files.

    If for any reason bypass compile is false, we do not write out the macro expansion. Only if we
    determine to bypass compile do we write out the macro expansions.
    """
    args = parse_args()

    # Determine if we should bypass compile based on modified patch files.
    if should_bypass_compile():
        evg_config = read_evg_config()
        if evg_config is None:
            print("Could not find ~/.evergreen.yml config file. Default compile bypass to false.")
            return

        api_server = "{url.scheme}://{url.netloc}".format(
            url=urlparse(evg_config.get("api_server_host")))
        revision_url = "{}/rest/v1/projects/{}/revisions/{}".format(api_server, args.project,
                                                                    args.revision)
        revisions = requests_get_json(revision_url)

        match = None
        prefix = "{}_{}_{}_".format(args.project, args.buildVariant, args.revision)
        # The "project" and "buildVariant" passed in may contain "-", but the "builds" listed from
        # Evergreen only contain "_". Replace the hyphens before searching for the build.
        prefix = prefix.replace("-", "_")
        build_id_pattern = re.compile(prefix)
        build_id = None
        for build_id in revisions["builds"]:
            # Find a suitable build_id
            match = build_id_pattern.search(build_id)
            if match:
                break
        else:
            print("Could not find build id for revision {} on project {}."
                  " Default compile bypass to false.".format(args.revision, args.project))
            return

        # Generate the compile task id.
        index = build_id.find(args.revision)
        compile_task_id = "{}compile_{}".format(build_id[:index], build_id[index:])
        task_url = "{}/rest/v1/tasks/{}".format(api_server, compile_task_id)
        # Get info on compile task of base commit.
        task = requests_get_json(task_url)
        if task is None or task["status"] != "success":
            print("Could not retrieve artifacts because the compile task {} for base commit"
                  " was not available. Default compile bypass to false.".format(compile_task_id))
            return

        # Get the compile task artifacts from REST API
        print("Fetching pre-existing artifacts from compile task {}".format(compile_task_id))
        artifacts = []
        for artifact in task["files"]:
            filename = os.path.basename(artifact["url"])
            if filename.startswith(build_id):
                print("Retrieving archive {}".format(filename))
                # This is the artifacts.tgz as referenced in evergreen.yml.
                try:
                    urllib.request.urlretrieve(artifact["url"], filename)
                except urllib.error.ContentTooShortError:
                    print("The artifact {} could not be completely downloaded. Default"
                          " compile bypass to false.".format(filename))
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
                    print("Extracting the following files from {0}...\n{1}".format(
                        filename, "\n".join(tarinfo.name for tarinfo in subdir)))
                    tar.extractall(members=subdir)
            elif filename.startswith("mongo-src"):
                print("Retrieving mongo source {}".format(filename))
                # This is the distsrc.[tgz|zip] as referenced in evergreen.yml.
                try:
                    urllib.request.urlretrieve(artifact["url"], filename)
                except urllib.error.ContentTooShortError:
                    print("The artifact {} could not be completely downloaded. Default"
                          " compile bypass to false.".format(filename))
                    return
                extension = os.path.splitext(filename)[1]
                distsrc_filename = "distsrc{}".format(extension)
                print("Renaming {} to {}".format(filename, distsrc_filename))
                os.rename(filename, distsrc_filename)
            else:
                print("Linking base artifact {} to this patch build".format(filename))
                # For other artifacts we just add their URLs to the JSON file to upload.
                files = {}
                files["name"] = artifact["name"]
                files["link"] = artifact["url"]
                files["visibility"] = "private"
                # Check the link exists, else raise an exception. Compile bypass is disabled.
                requests.head(artifact["url"]).raise_for_status()
                artifacts.append(files)

        # SERVER-21492 related issue where without running scons the jstests/libs/key1
        # and key2 files are not chmod to 0600. Need to change permissions here since we
        # bypass SCons.
        os.chmod("jstests/libs/key1", 0o600)
        os.chmod("jstests/libs/key2", 0o600)
        os.chmod("jstests/libs/keyForRollover", 0o600)

        # This is the artifacts.json file.
        write_out_artifacts(args.jsonArtifact, artifacts)

        # Need to apply these expansions for bypassing SCons.
        expansions = generate_bypass_expansions(args.project, args.buildVariant, args.revision,
                                                build_id)
        write_out_bypass_compile_expansions(args.outFile, **expansions)


if __name__ == "__main__":
    main()
